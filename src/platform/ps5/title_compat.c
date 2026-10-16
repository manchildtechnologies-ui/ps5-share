/* Small POSIX shims needed only by the native-title curl link. */
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef HUI_PS5_BUILD

/* I/O instrumentation counters defined by the app (worker thread only). */
extern unsigned long ps5share_poll_calls;
extern unsigned long ps5share_poll_ms;
extern unsigned long ps5share_select_calls;
extern unsigned long ps5share_select_ms;
extern unsigned long ps5share_send_calls;
extern unsigned long ps5share_send_eagain;
extern unsigned long long ps5share_send_bytes;
extern unsigned long ps5share_send_ms;
extern unsigned long ps5share_recv_calls;
extern unsigned long ps5share_recv_ms;
extern unsigned long ps5share_fcntl_nb;
extern unsigned long ps5share_fcntl_nbio_ok;
extern unsigned long ps5share_fcntl_getfl_fail;
extern unsigned long ps5share_fcntl_getfl_emul;

static unsigned long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
}

int pipe2(int fds[2], int flags) {
    if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(fds) < 0) return -1;
    for (int i = 0; i < 2; ++i) {
        if ((flags & O_CLOEXEC) && fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) goto fail;
        if (flags & O_NONBLOCK) {
            const int current = fcntl(fds[i], F_GETFL);
            if (current < 0 || fcntl(fds[i], F_SETFL, current | O_NONBLOCK) < 0) goto fail;
        }
    }
    return 0;
fail:
    close(fds[0]);
    close(fds[1]);
    return -1;
}

/* Current PS5 Share requests do not bind curl to a named interface. */
unsigned int if_nametoindex(const char *name) {
    (void)name;
    return 0;
}

/*
 * Neither poll() nor select() ever reports writability on this platform, so
 * uploads stall after the first TLS record. The link wraps poll references
 * from statically linked code to this implementation: POLLOUT is reported
 * immediately (a full send buffer just yields EAGAIN and the caller retries),
 * while readability/errors use a real select() wait, which does work.
 */
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout_ms) {
    extern int __real_poll(struct pollfd *fds, nfds_t count, int timeout_ms);
    extern int __real_select(int nfds, fd_set *readfds, fd_set *writefds,
                             fd_set *exceptfds, struct timeval *timeout);
    ++ps5share_poll_calls;
    const unsigned long start = now_ms();
    if (!fds && count != 0) {
        errno = EFAULT;
        ps5share_poll_ms += now_ms() - start;
        return -1;
    }
    int want_write = 0;
    for (nfds_t i = 0; i < count; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd >= 0 && (fds[i].events & POLLOUT)) want_write = 1;
    }
    /* Any writability wait returns immediately; pure read waits keep the timeout. */
    struct timeval immediate = {0, 0};
    struct timeval timeout;
    struct timeval *timeout_ptr;
    if (want_write) {
        timeout_ptr = &immediate;
    } else if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (long)(timeout_ms % 1000) * 1000L;
        timeout_ptr = &timeout;
    } else {
        timeout_ptr = NULL;
    }
    fd_set read_set, except_set;
    FD_ZERO(&read_set);
    FD_ZERO(&except_set);
    int maxfd = -1;
    for (nfds_t i = 0; i < count; ++i) {
        if (fds[i].fd < 0) continue;
        if (fds[i].fd >= FD_SETSIZE) {
            /* Beyond select range: fall back to the platform poll. */
            const int fallback = __real_poll(fds, count, timeout_ms);
            ps5share_poll_ms += now_ms() - start;
            return fallback;
        }
        if (fds[i].events & POLLIN) FD_SET(fds[i].fd, &read_set);
        FD_SET(fds[i].fd, &except_set);
        if (fds[i].fd > maxfd) maxfd = fds[i].fd;
    }
    if (maxfd >= 0) {
        /* __real_select: the shim's own waits stay out of the select stats. */
        const int ready = __real_select(maxfd + 1, &read_set, NULL, &except_set, timeout_ptr);
        if (ready < 0) {
            ps5share_poll_ms += now_ms() - start;
            return -1;
        }
        for (nfds_t i = 0; i < count; ++i) {
            if (fds[i].fd < 0) continue;
            if (FD_ISSET(fds[i].fd, &read_set) && (fds[i].events & POLLIN))
                fds[i].revents |= POLLIN;
            if (FD_ISSET(fds[i].fd, &except_set)) {
                if (fds[i].events & POLLPRI) fds[i].revents |= POLLPRI;
                /* A socket error surfaces here; report it so the caller
                 * retries the I/O and observes the real condition. */
                fds[i].revents |= POLLERR;
            }
        }
    } else if (!want_write) {
        /* No valid fds and no writability bypass: sleep like poll does. */
        const int slept = __real_select(0, NULL, NULL, NULL, timeout_ptr);
        if (slept < 0) {
            ps5share_poll_ms += now_ms() - start;
            return -1;
        }
    }
    int fired = 0;
    for (nfds_t i = 0; i < count; ++i) {
        if (fds[i].fd < 0) continue;
        if (want_write && (fds[i].events & POLLOUT)) fds[i].revents |= POLLOUT;
        if (fds[i].revents) ++fired;
    }
    ps5share_poll_ms += now_ms() - start;
    return fired;
}

/* I/O instrumentation: count and time every select/send/recv from linked code. */
int __wrap_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                  struct timeval *timeout) {
    extern int __real_select(int nfds, fd_set *readfds, fd_set *writefds,
                             fd_set *exceptfds, struct timeval *timeout);
    ++ps5share_select_calls;
    const unsigned long start = now_ms();
    const int result = __real_select(nfds, readfds, writefds, exceptfds, timeout);
    ps5share_select_ms += now_ms() - start;
    return result;
}

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags) {
    extern ssize_t __real_send(int fd, const void *buf, size_t len, int flags);
    ++ps5share_send_calls;
    const unsigned long start = now_ms();
    const ssize_t result = __real_send(fd, buf, len, flags);
    ps5share_send_ms += now_ms() - start;
    if (result > 0) ps5share_send_bytes += (unsigned long long)result;
    else if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ++ps5share_send_eagain;
    return result;
}

ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags) {
    extern ssize_t __real_recv(int fd, void *buf, size_t len, int flags);
    ++ps5share_recv_calls;
    const unsigned long start = now_ms();
    const ssize_t result = __real_recv(fd, buf, len, flags);
    ps5share_recv_ms += now_ms() - start;
    return result;
}

/*
 * SceNet's fcntl either ignores O_NONBLOCK or fails F_GETFL outright, so
 * curl's sockets stay blocking and its EAGAIN-driven state machine wedges in
 * recv mid-upload. Emulate the flag layer: apply every O_NONBLOCK request
 * through ioctl(FIONBIO), track it per fd, and answer F_GETFL from the
 * tracked state when the real call fails. Purely additive: fully working
 * fcntl implementations behave exactly as before.
 */
#define PS5SHARE_FD_TRACK_MAX 512
static unsigned char fd_nonblock[PS5SHARE_FD_TRACK_MAX];

static void track_nonblock(int fd, int on) {
    if (fd >= 0 && fd < PS5SHARE_FD_TRACK_MAX) fd_nonblock[fd] = on ? 1 : 0;
}

static int tracked_nonblock(int fd) {
    return fd >= 0 && fd < PS5SHARE_FD_TRACK_MAX && fd_nonblock[fd] != 0;
}

int __wrap_fcntl(int fd, int cmd, long arg) {
    extern int __real_fcntl(int fd, int cmd, long arg);
    if (cmd == F_GETFL) {
        const int result = __real_fcntl(fd, cmd, arg);
        if (result < 0) {
            ++ps5share_fcntl_getfl_fail;
            if (tracked_nonblock(fd)) {
                ++ps5share_fcntl_getfl_emul;
                return O_RDWR | O_NONBLOCK;
            }
        }
        return result;
    }
    if (cmd == F_SETFL) {
        if ((arg & O_NONBLOCK) != 0) {
            ++ps5share_fcntl_nb;
            int on = 1;
            const int io_result = ioctl(fd, FIONBIO, &on);
            const int result = __real_fcntl(fd, cmd, arg);
            if (io_result == 0) {
                ++ps5share_fcntl_nbio_ok;
                track_nonblock(fd, 1);
                return 0;
            }
            track_nonblock(fd, result == 0);
            return result;
        }
        int off = 0;
        ioctl(fd, FIONBIO, &off);
        track_nonblock(fd, 0);
        return __real_fcntl(fd, cmd, arg);
    }
    return __real_fcntl(fd, cmd, arg);
}

/*
 * libcurl references gmtime_r (cookie dates) and getpwuid_r (~/.netrc
 * lookup), which this title runtime provides in neither form. The share
 * client uses no cookies and no netrc, so a local gmtime_r (Howard
 * Hinnant's civil_from_days) and a "no such user" getpwuid_r suffice.
 */
struct tm *gmtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) {
        errno = EINVAL;
        return NULL;
    }
    const long long tt = (long long)*t;
    long long days = tt / 86400;
    long long rem = tt % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned long long doe = (unsigned long long)(z - era * 146097);
    const unsigned long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long year = (long long)yoe + era * 400;
    const unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned long long mp = (5 * doy + 2) / 153;
    const unsigned long long day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned long long month = mp + (mp < 10 ? 3 : -9);
    year += (month <= 2);
    int wday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday */
    if (wday < 0) wday += 7;
    int yday;
    if (month <= 2) {
        yday = (int)doy - 306;
    } else {
        const long long leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        yday = (int)doy + (leap ? 60 : 59);
    }
    out->tm_sec = (int)(rem % 60);
    out->tm_min = (int)((rem / 60) % 60);
    out->tm_hour = (int)(rem / 3600);
    out->tm_mday = (int)day;
    out->tm_mon = (int)(month - 1);
    out->tm_year = (int)(year - 1900);
    out->tm_wday = wday;
    out->tm_yday = yday;
    out->tm_isdst = 0;
    return out;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    (void)uid;
    (void)pwd;
    (void)buf;
    (void)buflen;
    if (result) *result = NULL;
    return 0; /* no such user: curl skips the netrc */
}

#endif /* HUI_PS5_BUILD */