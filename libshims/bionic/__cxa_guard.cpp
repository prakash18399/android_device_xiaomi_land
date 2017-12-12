#include "private/bionic_futex.h"

extern "C" int __cxa_guard_acquire(_guard_t* gv) {
  int old_value = atomic_load_explicit(&gv->state, memory_order_acquire);
  // In the common CONSTRUCTION_COMPLETE case we have to ensure that all the stores performed by
  // the construction function are observable on this CPU after we exit. A similar constraint may
  // apply in the CONSTRUCTION_NOT_YET_STARTED case with a prior abort.

  while (true) {
    if (old_value == CONSTRUCTION_COMPLETE) {
      return 0;
    } else if (old_value == CONSTRUCTION_NOT_YET_STARTED) {
      if (!atomic_compare_exchange_weak_explicit(&gv->state, &old_value,
                                                  CONSTRUCTION_UNDERWAY_WITHOUT_WAITER,
                                                  memory_order_acquire /* or relaxed in C++17 */,
                                                  memory_order_acquire)) {
        continue;
      }
      return 1;
    } else if (old_value == CONSTRUCTION_UNDERWAY_WITHOUT_WAITER) {
      if (!atomic_compare_exchange_weak_explicit(&gv->state, &old_value,
                                                 CONSTRUCTION_UNDERWAY_WITH_WAITER,
                                                 memory_order_acquire /* or relaxed in C++17 */,
                                                 memory_order_acquire)) {
        continue;
      }
    }

    __futex_wait_ex(&gv->state, false, CONSTRUCTION_UNDERWAY_WITH_WAITER, NULL);
    old_value = atomic_load_explicit(&gv->state, memory_order_acquire);
  }
}

