#include "private/bionic_futex.h"
#include "private/bionic_time_conversions.h"

struct pthread_cond_internal_t {
  atomic_uint state;

  bool process_shared() {
    return COND_IS_SHARED(atomic_load_explicit(&state, memory_order_relaxed));
  }

  int get_clock() {
    return COND_GET_CLOCK(atomic_load_explicit(&state, memory_order_relaxed));
  }

#if defined(__LP64__)
  char __reserved[44];
#endif
};

static int __pthread_cond_timedwait_relative(pthread_cond_internal_t* cond, pthread_mutex_t* mutex,
                                             const timespec* rel_timeout_or_null) {
  unsigned int old_state = atomic_load_explicit(&cond->state, memory_order_relaxed);

  pthread_mutex_unlock(mutex);
  int status = __futex_wait_ex(&cond->state, cond->process_shared(), old_state, rel_timeout_or_null);
  pthread_mutex_lock(mutex);

  if (status == -ETIMEDOUT) {
    return ETIMEDOUT;
  }
  return 0;
}

static int __pthread_cond_timedwait(pthread_cond_internal_t* cond, pthread_mutex_t* mutex,
                                    const timespec* abs_timeout_or_null, clockid_t clock) {
  timespec ts;
  timespec* rel_timeout = NULL;

  if (abs_timeout_or_null != NULL) {
    rel_timeout = &ts;
    if (!timespec_from_absolute_timespec(*rel_timeout, *abs_timeout_or_null, clock)) {
      return ETIMEDOUT;
    }
  }

  return __pthread_cond_timedwait_relative(cond, mutex, rel_timeout);
}

int pthread_cond_wait(pthread_cond_t* cond_interface, pthread_mutex_t* mutex) {
  pthread_cond_internal_t* cond = __get_internal_cond(cond_interface);
  return __pthread_cond_timedwait(cond, mutex, NULL, cond->get_clock());
}

int pthread_cond_timedwait(pthread_cond_t *cond_interface, pthread_mutex_t * mutex,
                           const timespec *abstime) {

  pthread_cond_internal_t* cond = __get_internal_cond(cond_interface);
  return __pthread_cond_timedwait(cond, mutex, abstime, cond->get_clock());
}

#if !defined(__LP64__)
// TODO: this exists only for backward binary compatibility on 32 bit platforms.
extern "C" int pthread_cond_timedwait_monotonic(pthread_cond_t* cond_interface,
                                                pthread_mutex_t* mutex,
                                                const timespec* abs_timeout) {

  return __pthread_cond_timedwait(__get_internal_cond(cond_interface), mutex, abs_timeout,
                                  CLOCK_MONOTONIC);
}

extern "C" int pthread_cond_timedwait_monotonic_np(pthread_cond_t* cond_interface,
                                                   pthread_mutex_t* mutex,
                                                   const timespec* abs_timeout) {
  return pthread_cond_timedwait_monotonic(cond_interface, mutex, abs_timeout);
}

extern "C" int pthread_cond_timedwait_relative_np(pthread_cond_t* cond_interface,
                                                  pthread_mutex_t* mutex,
                                                  const timespec* rel_timeout) {

  return __pthread_cond_timedwait_relative(__get_internal_cond(cond_interface), mutex, rel_timeout);
}

extern "C" int pthread_cond_timeout_np(pthread_cond_t* cond_interface,
                                       pthread_mutex_t* mutex, unsigned ms) {
  timespec ts;
  timespec_from_ms(ts, ms);
  return pthread_cond_timedwait_relative_np(cond_interface, mutex, &ts);
}
#endif // !defined(__LP64__)
