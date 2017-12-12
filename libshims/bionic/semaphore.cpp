#include "private/bionic_futex.h"
#include "private/bionic_time_conversions.h"

// "Increment" the value of a semaphore atomically and
// return its old value. Note that this implements
// the special case of "incrementing" any negative
// value to +1 directly.
//
// NOTE: The value will _not_ wrap above SEM_VALUE_MAX
static int __sem_inc(atomic_uint* sem_count_ptr) {
  unsigned int old_value = atomic_load_explicit(sem_count_ptr, memory_order_relaxed);
  unsigned int shared = old_value  & SEMCOUNT_SHARED_MASK;
  unsigned int new_value;

  // Use memory_order_seq_cst in atomic_compare_exchange operation to ensure all
  // memory access made before can be seen in other threads.
  // A release fence may be sufficient, but it is still in discussion whether
  // POSIX semaphores should provide sequential consistency.
  do {
    // Can't go higher than SEM_VALUE_MAX.
    if (SEMCOUNT_TO_VALUE(old_value) == SEM_VALUE_MAX) {
      break;
    }

    // If the counter is negative, go directly to one, otherwise just increment.
    if (SEMCOUNT_TO_VALUE(old_value) < 0) {
      new_value = SEMCOUNT_ONE | shared;
    } else {
      new_value = SEMCOUNT_INCREMENT(old_value) | shared;
    }
  } while (!atomic_compare_exchange_weak(sem_count_ptr, &old_value,
           new_value));

  return SEMCOUNT_TO_VALUE(old_value);
}

int sem_wait(sem_t* sem) {
  atomic_uint* sem_count_ptr = SEM_TO_ATOMIC_POINTER(sem);
  unsigned int shared = SEM_GET_SHARED(sem_count_ptr);

  while (true) {
    if (__sem_dec(sem_count_ptr) > 0) {
      return 0;
    }

    int result = __futex_wait_ex(sem_count_ptr, shared, shared | SEMCOUNT_MINUS_ONE, NULL);
    if (bionic_get_application_target_sdk_version() >= __ANDROID_API_N__) {
      if (result ==-EINTR) {
        errno = EINTR;
        return -1;
      }
    }
  }
}

int sem_timedwait(sem_t* sem, const timespec* abs_timeout) {
  atomic_uint* sem_count_ptr = SEM_TO_ATOMIC_POINTER(sem);

  // POSIX says we need to try to decrement the semaphore
  // before checking the timeout value. Note that if the
  // value is currently 0, __sem_trydec() does nothing.
  if (__sem_trydec(sem_count_ptr) > 0) {
    return 0;
  }

  // Check it as per POSIX.
  if (abs_timeout == NULL || abs_timeout->tv_sec < 0 || abs_timeout->tv_nsec < 0 || abs_timeout->tv_nsec >= NS_PER_S) {
    errno = EINVAL;
    return -1;
  }

  unsigned int shared = SEM_GET_SHARED(sem_count_ptr);

  while (true) {
    // POSIX mandates CLOCK_REALTIME here.
    timespec ts;
    if (!timespec_from_absolute_timespec(ts, *abs_timeout, CLOCK_REALTIME)) {
      errno = ETIMEDOUT;
      return -1;
    }

    // Try to grab the semaphore. If the value was 0, this will also change it to -1.
    if (__sem_dec(sem_count_ptr) > 0) {
      break;
    }

    // Contention detected. Wait for a wakeup event.
    int ret = __futex_wait_ex(sem_count_ptr, shared, shared | SEMCOUNT_MINUS_ONE, &ts);

    // Return in case of timeout or interrupt.
    if (ret == -ETIMEDOUT || ret == -EINTR) {
      errno = -ret;
      return -1;
    }
  }
  return 0;
}
