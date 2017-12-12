#include "private/bionic_futex.h"
#include "private/bionic_lock.h"
#include "private/bionic_time_conversions.h"

static int __pthread_rwlock_timedrdlock(pthread_rwlock_internal_t* rwlock,
                                        const timespec* abs_timeout_or_null) {

  if (atomic_load_explicit(&rwlock->writer_tid, memory_order_relaxed) == __get_thread()->tid) {
    return EDEADLK;
  }

  while (true) {
    int ret = __pthread_rwlock_tryrdlock(rwlock);
    if (ret == 0 || ret == EAGAIN) {
      return ret;
    }

    int old_state = atomic_load_explicit(&rwlock->state, memory_order_relaxed);
    if (__can_acquire_read_lock(old_state, rwlock->writer_nonrecursive_preferred)) {
      continue;
    }

    timespec ts;
    timespec* rel_timeout = NULL;

    if (abs_timeout_or_null != NULL) {
      rel_timeout = &ts;
      if (!timespec_from_absolute_timespec(*rel_timeout, *abs_timeout_or_null, CLOCK_REALTIME)) {
        return ETIMEDOUT;
      }
    }

    rwlock->pending_lock.lock();
    rwlock->pending_reader_count++;

    // We rely on the fact that all atomic exchange operations on the same object (here it is
    // rwlock->state) always appear to occur in a single total order. If the pending flag is added
    // before unlocking, the unlocking thread will wakeup the waiter. Otherwise, we will see the
    // state is unlocked and will not wait anymore.
    old_state = atomic_fetch_or_explicit(&rwlock->state, STATE_HAVE_PENDING_READERS_FLAG,
                                         memory_order_relaxed);

    int old_serial = rwlock->pending_reader_wakeup_serial;
    rwlock->pending_lock.unlock();

    int futex_ret = 0;
    if (!__can_acquire_read_lock(old_state, rwlock->writer_nonrecursive_preferred)) {
      futex_ret = __futex_wait_ex(&rwlock->pending_reader_wakeup_serial, rwlock->pshared,
                                  old_serial, rel_timeout);
    }

    rwlock->pending_lock.lock();
    rwlock->pending_reader_count--;
    if (rwlock->pending_reader_count == 0) {
      atomic_fetch_and_explicit(&rwlock->state, ~STATE_HAVE_PENDING_READERS_FLAG,
                                memory_order_relaxed);
    }
    rwlock->pending_lock.unlock();

    if (futex_ret == -ETIMEDOUT) {
      return ETIMEDOUT;
    }
  }
}

static int __pthread_rwlock_timedwrlock(pthread_rwlock_internal_t* rwlock,
                                        const timespec* abs_timeout_or_null) {

  if (atomic_load_explicit(&rwlock->writer_tid, memory_order_relaxed) == __get_thread()->tid) {
    return EDEADLK;
  }
  while (true) {
    int ret = __pthread_rwlock_trywrlock(rwlock);
    if (ret == 0) {
      return ret;
    }

    int old_state = atomic_load_explicit(&rwlock->state, memory_order_relaxed);
    if (__can_acquire_write_lock(old_state)) {
      continue;
    }

    timespec ts;
    timespec* rel_timeout = NULL;

    if (abs_timeout_or_null != NULL) {
      rel_timeout = &ts;
      if (!timespec_from_absolute_timespec(*rel_timeout, *abs_timeout_or_null, CLOCK_REALTIME)) {
        return ETIMEDOUT;
      }
    }

    rwlock->pending_lock.lock();
    rwlock->pending_writer_count++;

    old_state = atomic_fetch_or_explicit(&rwlock->state, STATE_HAVE_PENDING_WRITERS_FLAG,
                                         memory_order_relaxed);

    int old_serial = rwlock->pending_writer_wakeup_serial;
    rwlock->pending_lock.unlock();

    int futex_ret = 0;
    if (!__can_acquire_write_lock(old_state)) {
      futex_ret = __futex_wait_ex(&rwlock->pending_writer_wakeup_serial, rwlock->pshared,
                                  old_serial, rel_timeout);
    }

    rwlock->pending_lock.lock();
    rwlock->pending_writer_count--;
    if (rwlock->pending_writer_count == 0) {
      atomic_fetch_and_explicit(&rwlock->state, ~STATE_HAVE_PENDING_WRITERS_FLAG,
                                memory_order_relaxed);
    }
    rwlock->pending_lock.unlock();

    if (futex_ret == -ETIMEDOUT) {
      return ETIMEDOUT;
    }
  }
}

int pthread_rwlock_rdlock(pthread_rwlock_t* rwlock_interface) {
  pthread_rwlock_internal_t* rwlock = __get_internal_rwlock(rwlock_interface);
  // Avoid slowing down fast path of rdlock.
  if (__predict_true(__pthread_rwlock_tryrdlock(rwlock) == 0)) {
    return 0;
  }
  return __pthread_rwlock_timedrdlock(rwlock, NULL);
}

int pthread_rwlock_wrlock(pthread_rwlock_t* rwlock_interface) {
  pthread_rwlock_internal_t* rwlock = __get_internal_rwlock(rwlock_interface);
  // Avoid slowing down fast path of wrlock.
  if (__predict_true(__pthread_rwlock_trywrlock(rwlock) == 0)) {
    return 0;
  }
  return __pthread_rwlock_timedwrlock(rwlock, NULL);
}
