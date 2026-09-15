/*
 * POSIX clock surface for the pure-Zephyr 4.4 build.
 *
 * Zephyr 4.4 re-parented CONFIG_POSIX_TIMERS under the
 * CONFIG_POSIX_SYSTEM_INTERFACES umbrella, which routes <time.h>/
 * <unistd.h>/... to Zephyr's include/zephyr/posix/ headers app-wide
 * (-I include/zephyr/posix on every TU). That shadows the libc's own
 * POSIX declarations and broke the colcon build of rcutils (implicit
 * isatty/fork/execvp declarations under gcc >= 14) — the same
 * header-routing family of problems that CONFIG_POSIX_API caused on
 * NCS (see prj.conf). The app keeps all POSIX header routing OFF and
 * instead provides the handful of clock functions that micro-ROS
 * actually needs, mirroring zephyr/lib/posix/options/clock.c
 * (Copyright (c) 2018 Intel Corporation; Apache-2.0, Zephyr
 * contributors) on top of the always-compiled kernel sys_clock_* API
 * (lib/os/clock.c).
 *
 * picolibc's <time.h> declares all of these (default visibility); it
 * just does not define them.
 */

#include <errno.h>
#include <time.h>

#include <sys/types.h> /* useconds_t (not pulled in by <time.h>) */

#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>

int clock_gettime(clockid_t clock_id, struct timespec *ts)
{
	int ret = sys_clock_gettime(sys_clock_from_clockid((int)clock_id), ts);

	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	return 0;
}

int clock_getres(clockid_t clock_id, struct timespec *res)
{
	BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC > 0 &&
		     CONFIG_SYS_CLOCK_TICKS_PER_SEC <= NSEC_PER_SEC,
		     "CONFIG_SYS_CLOCK_TICKS_PER_SEC must be > 0 and <= NSEC_PER_SEC");

	if (!(clock_id == CLOCK_MONOTONIC || clock_id == CLOCK_REALTIME)) {
		errno = EINVAL;
		return -1;
	}

	if (res != NULL) {
		*res = (struct timespec){
			.tv_sec = 0,
			.tv_nsec = NSEC_PER_SEC / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
		};
	}

	return 0;
}

int clock_settime(clockid_t clock_id, const struct timespec *tp)
{
	int ret = sys_clock_settime(sys_clock_from_clockid((int)clock_id), tp);

	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	return 0;
}

/*
 * usleep() was removed in POSIX Issue 7; kept for compatibility.
 */
int usleep(useconds_t useconds)
{
	int32_t rem;

	if (useconds >= USEC_PER_SEC) {
		errno = EINVAL;
		return -1;
	}

	rem = k_usleep(useconds);
	__ASSERT_NO_MSG(rem >= 0);
	if (rem > 0) {
		/* sleep was interrupted by a call to k_wakeup() */
		errno = EINTR;
		return -1;
	}

	return 0;
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
	int ret = sys_clock_nanosleep(SYS_CLOCK_REALTIME, 0, rqtp, rmtp);

	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	return 0;
}