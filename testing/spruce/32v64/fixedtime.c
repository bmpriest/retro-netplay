/* LD_PRELOAD: pin the wall clock so cores that stamp time(0) into state (RTC
 * carts, gambatte's power-on base time) compare across runs. Monotonic clocks
 * are left alone. time_t and suseconds_t are both 'long' on armhf and aarch64. */
#define _GNU_SOURCE
#include <time.h>
#include <dlfcn.h>
#define T 1700000000L
time_t time(time_t* t) { if (t) *t = T; return T; }
int gettimeofday(long* tv, void* tz) { (void)tz; if (tv) { tv[0] = T; tv[1] = 0; } return 0; }
int clock_gettime(clockid_t id, struct timespec* ts) {
	static int (*real)(clockid_t, struct timespec*);
	if (id == CLOCK_REALTIME || id == CLOCK_REALTIME_COARSE) { ts->tv_sec = T; ts->tv_nsec = 0; return 0; }
	if (!real) real = (int(*)(clockid_t, struct timespec*))dlsym(RTLD_NEXT, "clock_gettime");
	return real(id, ts);
}
