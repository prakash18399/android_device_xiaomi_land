#include "private/bionic_futex.h"

// According to POSIX standard, pthread_barrier_wait() synchronizes memory between participating
// threads. It means all memory operations made by participating threads before calling
// pthread_barrier_wait() can be seen by all participating threads after the function call.
// We establish this by making a happens-before relation between all threads entering the barrier
// with the last thread entering the barrier, and a happens-before relation between the last
// thread entering the barrier with all threads leaving the barrier.
int pthread_barrier_wait(pthread_barrier_t* barrier_interface) {
  pthread_barrier_internal_t* barrier = __get_internal_barrier(barrier_interface);

  // Wait until all threads for the previous cycle have left the barrier. This is needed
  // as a participating thread can call pthread_barrier_wait() again before other
  // threads have left the barrier. Use acquire operation here to synchronize with
  // the last thread leaving the previous cycle, so we can read correct wait_count below.
  while(atomic_load_explicit(&barrier->state, memory_order_acquire) == RELEASE) {
    __futex_wait_ex(&barrier->state, barrier->pshared, RELEASE, nullptr);
  }

  uint32_t prev_wait_count = atomic_load_explicit(&barrier->wait_count, memory_order_relaxed);
  while (true) {
    // It happens when there are more than [init_count] threads trying to enter the barrier
    // at one cycle. We read the POSIX standard as disallowing this, since additional arriving
    // threads are not synchronized with respect to the barrier reset. We also don't know of
    // any reasonable cases in which this would be intentional.
    if (prev_wait_count >= barrier->init_count) {
      return EINVAL;
    }
    // Use memory_order_acq_rel operation here to synchronize between all threads entering
    // the barrier with the last thread entering the barrier.
    if (atomic_compare_exchange_weak_explicit(&barrier->wait_count, &prev_wait_count,
                                              prev_wait_count + 1u, memory_order_acq_rel,
                                              memory_order_relaxed)) {
      break;
    }
  }

  int result = 0;
  if (prev_wait_count + 1 == barrier->init_count) {
    result = PTHREAD_BARRIER_SERIAL_THREAD;
    if (prev_wait_count != 0) {
      // Use release operation here to synchronize between the last thread entering the
      // barrier with all threads leaving the barrier.
      atomic_store_explicit(&barrier->state, RELEASE, memory_order_release);
      __futex_wake_ex(&barrier->state, barrier->pshared, prev_wait_count);
    }
  } else {
    // Use acquire operation here to synchronize between the last thread entering the
    // barrier with all threads leaving the barrier.
    while (atomic_load_explicit(&barrier->state, memory_order_acquire) == WAIT) {
      __futex_wait_ex(&barrier->state, barrier->pshared, WAIT, nullptr);
    }
  }
  // Use release operation here to make it not reordered with previous operations.
  if (atomic_fetch_sub_explicit(&barrier->wait_count, 1, memory_order_release) == 1) {
    // Use release operation here to synchronize with threads entering the barrier for
    // the next cycle, or the thread calling pthread_barrier_destroy().
    atomic_store_explicit(&barrier->state, WAIT, memory_order_release);
    __futex_wake_ex(&barrier->state, barrier->pshared, barrier->init_count);
  }
  return result;
}

int pthread_barrier_destroy(pthread_barrier_t* barrier_interface) {
  pthread_barrier_internal_t* barrier = __get_internal_barrier(barrier_interface);
  if (barrier->init_count == 0) {
    return EINVAL;
  }
  // Use acquire operation here to synchronize with the last thread leaving the barrier.
  // So we can read correct wait_count below.
  while (atomic_load_explicit(&barrier->state, memory_order_acquire) == RELEASE) {
    __futex_wait_ex(&barrier->state, barrier->pshared, RELEASE, nullptr);
  }
  if (atomic_load_explicit(&barrier->wait_count, memory_order_relaxed) != 0) {
    return EBUSY;
  }
  barrier->init_count = 0;
  return 0;
}
