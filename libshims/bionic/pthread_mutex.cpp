#include "private/bionic_futex.h"
#include "private/bionic_time_conversions.h"

/*
 * Lock a mutex of type NORMAL.
 *
 * As noted above, there are three states:
 *   0 (unlocked, no contention)
 *   1 (locked, no contention)
 *   2 (locked, contention)
 *
 * Non-recursive mutexes don't use the thread-id or counter fields, and the
 * "type" value is zero, so the only bits that will be set are the ones in
 * the lock state field.
 */
static inline __always_inline int __pthread_normal_mutex_lock(pthread_mutex_internal_t* mutex,
                                                              uint16_t shared,
                                                              const timespec* abs_timeout_or_null,
                                                              clockid_t clock) {
    if (__predict_true(__pthread_normal_mutex_trylock(mutex, shared) == 0)) {
        return 0;
    }

    ScopedTrace trace("Contending for pthread mutex");

    const uint16_t unlocked           = shared | MUTEX_STATE_BITS_UNLOCKED;
    const uint16_t locked_contended = shared | MUTEX_STATE_BITS_LOCKED_CONTENDED;

    // We want to go to sleep until the mutex is available, which requires
    // promoting it to locked_contended. We need to swap in the new state
    // and then wait until somebody wakes us up.
    // An atomic_exchange is used to compete with other threads for the lock.
    // If it returns unlocked, we have acquired the lock, otherwise another
    // thread still holds the lock and we should wait again.
    // If lock is acquired, an acquire fence is needed to make all memory accesses
    // made by other threads visible to the current CPU.
    while (atomic_exchange_explicit(&mutex->state, locked_contended,
                                    memory_order_acquire) != unlocked) {
        timespec ts;
        timespec* rel_timeout = NULL;
        if (abs_timeout_or_null != NULL) {
            rel_timeout = &ts;
            if (!timespec_from_absolute_timespec(*rel_timeout, *abs_timeout_or_null, clock)) {
                return ETIMEDOUT;
            }
        }
        if (__futex_wait_ex(&mutex->state, shared, locked_contended, rel_timeout) == -ETIMEDOUT) {
            return ETIMEDOUT;
        }
    }
    return 0;
}

tatic inline __always_inline int __recursive_or_errorcheck_mutex_wait(
                                                      pthread_mutex_internal_t* mutex,
                                                      uint16_t shared,
                                                      uint16_t old_state,
                                                      const timespec* rel_timeout) {
// __futex_wait always waits on a 32-bit value. But state is 16-bit. For a normal mutex, the owner_tid
// field in mutex is not used. On 64-bit devices, the __pad field in mutex is not used.
// But when a recursive or errorcheck mutex is used on 32-bit devices, we need to add the
// owner_tid value in the value argument for __futex_wait, otherwise we may always get EAGAIN error.

#if defined(__LP64__)
  return __futex_wait_ex(&mutex->state, shared, old_state, rel_timeout);

#else
  // This implementation works only when the layout of pthread_mutex_internal_t matches below expectation.
  // And it is based on the assumption that Android is always in little-endian devices.
  static_assert(offsetof(pthread_mutex_internal_t, state) == 0, "");
  static_assert(offsetof(pthread_mutex_internal_t, owner_tid) == 2, "");

  uint32_t owner_tid = atomic_load_explicit(&mutex->owner_tid, memory_order_relaxed);
  return __futex_wait_ex(&mutex->state, shared, (owner_tid << 16) | old_state, rel_timeout);
#endif
}

static int __pthread_mutex_lock_with_timeout(pthread_mutex_internal_t* mutex,
                                           const timespec* abs_timeout_or_null, clockid_t clock) {
    uint16_t old_state = atomic_load_explicit(&mutex->state, memory_order_relaxed);
    uint16_t mtype = (old_state & MUTEX_TYPE_MASK);
    uint16_t shared = (old_state & MUTEX_SHARED_MASK);

    // Handle common case first.
    if ( __predict_true(mtype == MUTEX_TYPE_BITS_NORMAL) ) {
        return __pthread_normal_mutex_lock(mutex, shared, abs_timeout_or_null, clock);
    }

    // Do we already own this recursive or error-check mutex?
    pid_t tid = __get_thread()->tid;
    if (tid == atomic_load_explicit(&mutex->owner_tid, memory_order_relaxed)) {
        if (mtype == MUTEX_TYPE_BITS_ERRORCHECK) {
            return EDEADLK;
        }
        return __recursive_increment(mutex, old_state);
    }

    const uint16_t unlocked           = mtype | shared | MUTEX_STATE_BITS_UNLOCKED;
    const uint16_t locked_uncontended = mtype | shared | MUTEX_STATE_BITS_LOCKED_UNCONTENDED;
    const uint16_t locked_contended   = mtype | shared | MUTEX_STATE_BITS_LOCKED_CONTENDED;

    // First, if the mutex is unlocked, try to quickly acquire it.
    // In the optimistic case where this works, set the state to locked_uncontended.
    if (old_state == unlocked) {
        // If exchanged successfully, an acquire fence is required to make
        // all memory accesses made by other threads visible to the current CPU.
        if (__predict_true(atomic_compare_exchange_strong_explicit(&mutex->state, &old_state,
                             locked_uncontended, memory_order_acquire, memory_order_relaxed))) {
            atomic_store_explicit(&mutex->owner_tid, tid, memory_order_relaxed);
            return 0;
        }
    }

    ScopedTrace trace("Contending for pthread mutex");

    while (true) {
        if (old_state == unlocked) {
            // NOTE: We put the state to locked_contended since we _know_ there
            // is contention when we are in this loop. This ensures all waiters
            // will be unlocked.

            // If exchanged successfully, an acquire fence is required to make
            // all memory accesses made by other threads visible to the current CPU.
            if (__predict_true(atomic_compare_exchange_weak_explicit(&mutex->state,
                                                                     &old_state, locked_contended,
                                                                     memory_order_acquire,
                                                                     memory_order_relaxed))) {
                atomic_store_explicit(&mutex->owner_tid, tid, memory_order_relaxed);
                return 0;
            }
            continue;
        } else if (MUTEX_STATE_BITS_IS_LOCKED_UNCONTENDED(old_state)) {
            // We should set it to locked_contended beforing going to sleep. This can make
            // sure waiters will be woken up eventually.

            int new_state = MUTEX_STATE_BITS_FLIP_CONTENTION(old_state);
            if (__predict_false(!atomic_compare_exchange_weak_explicit(&mutex->state,
                                                                       &old_state, new_state,
                                                                       memory_order_relaxed,
                                                                       memory_order_relaxed))) {
                continue;
            }
            old_state = new_state;
        }

        // We are in locked_contended state, sleep until someone wakes us up.
        timespec ts;
        timespec* rel_timeout = NULL;
        if (abs_timeout_or_null != NULL) {
            rel_timeout = &ts;
            if (!timespec_from_absolute_timespec(*rel_timeout, *abs_timeout_or_null, clock)) {
                return ETIMEDOUT;
            }
        }
        if (__recursive_or_errorcheck_mutex_wait(mutex, shared, old_state, rel_timeout) == -ETIMEDOUT) {
            return ETIMEDOUT;
        }
        old_state = atomic_load_explicit(&mutex->state, memory_order_relaxed);
    }
}

int pthread_mutex_lock(pthread_mutex_t* mutex_interface) {
#if !defined(__LP64__)
    // Some apps depend on being able to pass NULL as a mutex and get EINVAL
    // back. Don't need to worry about it for LP64 since the ABI is brand new,
    // but keep compatibility for LP32. http://b/19995172.
    if (mutex_interface == NULL) {
        return EINVAL;
    }
#endif

    pthread_mutex_internal_t* mutex = __get_internal_mutex(mutex_interface);

    uint16_t old_state = atomic_load_explicit(&mutex->state, memory_order_relaxed);
    uint16_t mtype = (old_state & MUTEX_TYPE_MASK);
    uint16_t shared = (old_state & MUTEX_SHARED_MASK);
    // Avoid slowing down fast path of normal mutex lock operation.
    if (__predict_true(mtype == MUTEX_TYPE_BITS_NORMAL)) {
      if (__predict_true(__pthread_normal_mutex_trylock(mutex, shared) == 0)) {
        return 0;
      }
    }
    return __pthread_mutex_lock_with_timeout(mutex, NULL, 0);
}

int pthread_mutex_trylock(pthread_mutex_t* mutex_interface) {
    pthread_mutex_internal_t* mutex = __get_internal_mutex(mutex_interface);

    uint16_t old_state = atomic_load_explicit(&mutex->state, memory_order_relaxed);
    uint16_t mtype  = (old_state & MUTEX_TYPE_MASK);
    uint16_t shared = (old_state & MUTEX_SHARED_MASK);

    const uint16_t unlocked           = mtype | shared | MUTEX_STATE_BITS_UNLOCKED;
    const uint16_t locked_uncontended = mtype | shared | MUTEX_STATE_BITS_LOCKED_UNCONTENDED;

    // Handle common case first.
    if (__predict_true(mtype == MUTEX_TYPE_BITS_NORMAL)) {
        return __pthread_normal_mutex_trylock(mutex, shared);
    }

    // Do we already own this recursive or error-check mutex?
    pid_t tid = __get_thread()->tid;
    if (tid == atomic_load_explicit(&mutex->owner_tid, memory_order_relaxed)) {
        if (mtype == MUTEX_TYPE_BITS_ERRORCHECK) {
            return EBUSY;
        }
        return __recursive_increment(mutex, old_state);
    }

    // Same as pthread_mutex_lock, except that we don't want to wait, and
    // the only operation that can succeed is a single compare_exchange to acquire the
    // lock if it is released / not owned by anyone. No need for a complex loop.
    // If exchanged successfully, an acquire fence is required to make
    // all memory accesses made by other threads visible to the current CPU.
    old_state = unlocked;
    if (__predict_true(atomic_compare_exchange_strong_explicit(&mutex->state, &old_state,
                                                               locked_uncontended,
                                                               memory_order_acquire,
                                                               memory_order_relaxed))) {
        atomic_store_explicit(&mutex->owner_tid, tid, memory_order_relaxed);
        return 0;
    }
    return EBUSY;
}

#if !defined(__LP64__)
extern "C" int pthread_mutex_lock_timeout_np(pthread_mutex_t* mutex_interface, unsigned ms) {
    timespec abs_timeout;
    clock_gettime(CLOCK_MONOTONIC, &abs_timeout);
    abs_timeout.tv_sec  += ms / 1000;
    abs_timeout.tv_nsec += (ms % 1000) * 1000000;
    if (abs_timeout.tv_nsec >= NS_PER_S) {
        abs_timeout.tv_sec++;
        abs_timeout.tv_nsec -= NS_PER_S;
    }

    int error = __pthread_mutex_lock_with_timeout(__get_internal_mutex(mutex_interface),
                                                  &abs_timeout, CLOCK_MONOTONIC);
    if (error == ETIMEDOUT) {
        error = EBUSY;
    }
    return error;
}
#endif

int pthread_mutex_timedlock(pthread_mutex_t* mutex_interface, const timespec* abs_timeout) {
    return __pthread_mutex_lock_with_timeout(__get_internal_mutex(mutex_interface),
                                             abs_timeout, CLOCK_REALTIME);
}
