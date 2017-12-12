#include "private/bionic_time_conversions.h"

// Initializes 'ts' with the difference between 'abs_ts' and the current time
// according to 'clock'. Returns false if abstime already expired, true otherwise.
bool timespec_from_absolute_timespec(timespec& ts, const timespec& abs_ts, clockid_t clock) {
  clock_gettime(clock, &ts);
  ts.tv_sec = abs_ts.tv_sec - ts.tv_sec;
  ts.tv_nsec = abs_ts.tv_nsec - ts.tv_nsec;
  if (ts.tv_nsec < 0) {
    ts.tv_sec--;
    ts.tv_nsec += NS_PER_S;
  }
  if (ts.tv_nsec < 0 || ts.tv_sec < 0) {
    return false;
  }
  return true;
}
