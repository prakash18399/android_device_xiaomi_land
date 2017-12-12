// Lock is used in places like pthread_rwlock_t, which can be initialized without calling
// an initialization function. So make sure Lock can be initialized by setting its memory to 0.
class Lock {
 private:
  enum LockState {
    Unlocked = 0,
    LockedWithoutWaiter,
    LockedWithWaiter,
  };
  _Atomic(LockState) state;
  bool process_shared;

 public:
  void init(bool process_shared) {
    atomic_init(&state, Unlocked);
    this->process_shared = process_shared;
  }

  bool trylock() {
    LockState old_state = Unlocked;
    return __predict_true(atomic_compare_exchange_strong_explicit(&state, &old_state,
                        LockedWithoutWaiter, memory_order_acquire, memory_order_relaxed));
  }

  void lock() {
    LockState old_state = Unlocked;
    if (__predict_true(atomic_compare_exchange_strong_explicit(&state, &old_state,
                         LockedWithoutWaiter, memory_order_acquire, memory_order_relaxed))) {
      return;
    }
    while (atomic_exchange_explicit(&state, LockedWithWaiter, memory_order_acquire) != Unlocked) {
      // TODO: As the critical section is brief, it is a better choice to spin a few times befor sleeping.
      __futex_wait_ex(&state, process_shared, LockedWithWaiter, NULL);
    }
    return;
  }

  void unlock() {
    if (atomic_exchange_explicit(&state, Unlocked, memory_order_release) == LockedWithWaiter) {
      __futex_wake_ex(&state, process_shared, 1);
    }
  }
};
