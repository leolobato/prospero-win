/* SPDX-License-Identifier: LGPL-2.1-or-later
 * GStreamer/GLib interfaces missing from the title's public stubs. Do not
 * link SDK syscalls.o: its raw syscall instructions are payload-only.
 * GStreamer calls ppoll with no signal mask; atomic mask switching is not
 * emulated. Dynamic plugin scanning and child-process discovery are disabled.
 */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static int valid_timespec(const struct timespec *time)
{
    return time && time->tv_sec >= 0 && time->tv_nsec >= 0 && time->tv_nsec < 1000000000;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remaining)
{
    struct timespec now, delay;
    int error, saved_errno = errno;
    if (!valid_timespec(request) || (flags & ~TIMER_ABSTIME)) return EINVAL;
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return ENOTSUP;
    do {
        delay = *request;
        if (flags & TIMER_ABSTIME) {
            if (clock_gettime(clock, &now)) {
                error = errno; errno = saved_errno; return error;
            }
            if (now.tv_sec > request->tv_sec ||
                (now.tv_sec == request->tv_sec && now.tv_nsec >= request->tv_nsec)) return 0;
            delay.tv_sec -= now.tv_sec;
            delay.tv_nsec -= now.tv_nsec;
            if (delay.tv_nsec < 0) { --delay.tv_sec; delay.tv_nsec += 1000000000; }
        }
        if (nanosleep(&delay, (flags & TIMER_ABSTIME) ? NULL : remaining)) {
            error = errno; errno = saved_errno; return error;
        }
        /* Recheck an absolute deadline if the realtime clock moved backward. */
    } while (flags & TIMER_ABSTIME);
    errno = saved_errno;
    return 0;
}

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask)
{
    int milliseconds = -1;
    if (mask) { errno = ENOTSUP; return -1; }
    if (timeout) {
        int64_t rounded;
        if (!valid_timespec(timeout)) { errno = EINVAL; return -1; }
        if (timeout->tv_sec > INT_MAX / 1000) { errno = EOVERFLOW; return -1; }
        rounded = (int64_t)timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
        if (rounded > INT_MAX) { errno = EOVERFLOW; return -1; }
        milliseconds = (int)rounded;
    }
    return poll(fds, count, milliseconds);
}

void closefrom(int lowfd)
{
    struct rlimit limit;
    if (lowfd < 0) lowfd = 0;
    if (getrlimit(RLIMIT_NOFILE, &limit)) return;
    if (limit.rlim_cur > INT_MAX) { errno = EOVERFLOW; return; }
    for (int fd = lowfd; fd < (int)limit.rlim_cur; ++fd) close(fd);
}

/* Title processes cannot fork. GStreamer's scanner is disabled, and any
 * remaining optional process creation must report that it is unsupported. */
pid_t fork(void)
{
    errno = ENOSYS;
    return -1;
}

/* Preserve Wine's virtual-symlink handling; libkernel_sys is unavailable. */
extern ssize_t __wrap_readlink(const char *, char *, size_t);
ssize_t readlink(const char *path, char *buffer, size_t size)
{
    return __wrap_readlink(path, buffer, size);
}
