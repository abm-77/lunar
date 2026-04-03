#include <errno.h>
#include <stdint.h>
#include <time.h>

#define NS_IN_SEC 1000000000ull

#if defined(__APPLE__)
#include <mach/mach_time.h>

static mach_timebase_info_data_t g_um_timebase;
static int32_t g_um_timebase_init = 0;

static void um_init_timebase(void) {
  if (!g_um_timebase_init) {
    mach_timebase_info(&g_um_timebase);
    g_um_timebase_init = 1;
  }
}

uint64_t rt_time_now_monotonic_ns(void) {
  um_init_timebase();
  uint64_t t = mach_absolute_time();
  return t * (uint64_t)g_um_timebase.numer / (uint64_t)g_um_timebase.denom;
}
#else
uint64_t rt_time_now_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * NS_IN_SEC + (uint64_t)ts.tv_nsec;
}
#endif

uint64_t rt_time_now_wall_utc_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * NS_IN_SEC + (uint64_t)ts.tv_nsec;
}

void rt_time_sleep_ns(uint64_t ns) {
  if (ns == 0) return;

  struct timespec req;
  req.tv_sec = (time_t)(ns / NS_IN_SEC);
  req.tv_nsec = (long)(ns % NS_IN_SEC);

  for (;;) {
    struct timespec rem;
    int32_t rc = nanosleep(&req, &rem);
    if (rc == 0) return;
    if (errno == EINTR) {
      req = rem;
      continue;
    }
    return;
  }
}
