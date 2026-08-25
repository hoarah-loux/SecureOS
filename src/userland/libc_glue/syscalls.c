// newlib retargeting layer for SecureOS.
//
// newlib is configured with --disable-newlib-supplied-syscalls, so it provides
// no default stubs at all: every underscore-prefixed primitive it calls has to
// come from here. Anything missing shows up as a link error rather than as a
// mysterious runtime failure, which is the point of that configure flag.
//
// Two structural notes:
//
//   - The kernel has no errno. Every syscall returns a flat -1 (see the
//     comment in hdr/syscall_abi.h and the dispatcher in
//     src/kernel/interrupts/syscall.c), so there is no way to tell "no such
//     file" from "fd table full". The errno values set below are therefore
//     plausible rather than accurate. Anything that needs to distinguish
//     failures properly needs the kernel to grow real error returns first.
//
//   - There is no fork/exec/wait in the kernel, and no signals. Those stubs
//     fail unconditionally rather than pretending to work.

#include <errno.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "secureos_syscall.h"

// newlib references this directly for getenv() and friends.
static char *__empty_env[] = { 0 };
char **environ = __empty_env;

// ---------------- file I/O ----------------
//
// fds 0/1/2 are pre-populated by create_proc(); real files land in slots 3..15
// (PROC_FD_BASE..PROC_MAX_OPEN_FILES, hdr/p.h). Slot 3 is special: the loader
// pins the executable there and the process's own text and data are demand-
// paged through it for the whole life of the process. Closing it would tear
// out the backing store this code is running from.

int _open(const char *name, int flags, ...)
{
    long rc = __sys3(SYS_open, name, flags, 0);
    if (rc < 0) { errno = ENOENT; return -1; }
    return (int)rc;
}

int _close(int fd)
{
    long rc = __sys1(SYS_close, fd);
    if (rc < 0) { errno = EBADF; return -1; }
    return 0;
}

_ssize_t _read(int fd, void *buf, size_t len)
{
    long rc = __sys3(SYS_read, fd, buf, len);
    if (rc < 0) { errno = EIO; return -1; }
    return (_ssize_t)rc;
}

_ssize_t _write(int fd, const void *buf, size_t len)
{
    long rc = __sys3(SYS_write, fd, buf, len);
    if (rc < 0) { errno = EIO; return -1; }
    return (_ssize_t)rc;
}

_off_t _lseek(int fd, _off_t offset, int whence)
{
    long rc = __sys3(SYS_lseek, fd, offset, whence);
    if (rc < 0) { errno = EINVAL; return (_off_t)-1; }
    return (_off_t)rc;
}

// stdio calls this to decide buffering strategy and to size reads. The kernel
// has no stat, so recover a regular file's length the only way available:
// seek to the end, note the offset, seek back.
int _fstat(int fd, struct stat *st)
{
    if (st == 0) { errno = EFAULT; return -1; }

    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        st->st_mode = S_IFCHR;
        st->st_size = 0;
        st->st_blksize = 1024;
        return 0;
    }

    _off_t cur = (_off_t)__sys3(SYS_lseek, fd, 0, SEEK_CUR);
    if (cur < 0) { errno = EBADF; return -1; }

    _off_t end = (_off_t)__sys3(SYS_lseek, fd, 0, SEEK_END);
    if (end < 0) { errno = EBADF; return -1; }

    (void)__sys3(SYS_lseek, fd, cur, SEEK_SET);

    st->st_mode = S_IFREG;
    st->st_size = end;
    st->st_blksize = 1024;
    return 0;
}

int _isatty(int fd)
{
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

int _stat(const char *name, struct stat *st)
{
    int fd = _open(name, 0);
    if (fd < 0) { return -1; }
    int rc = _fstat(fd, st);
    _close(fd);
    return rc;
}

// ---------------- heap ----------------
//
// TEMPORARY: a fixed .bss arena, not SYS_brk.
//
// SYS_brk is real now (src/kernel/interrupts/syscall.c). The kernel reserves
// one 64 MiB anonymous demand-paged region per process at load time and brk()
// moves a cursor inside it, so growing the heap costs nothing until the pages
// are touched and there is no mapping work on this path at all.
//
// This replaces a fixed 256 KiB .bss arena that stood in while SYS_brk was a
// stub. That arena could not have carried doom, whose zone allocator asks for
// 16 MiB up front.
//
// The contract SYS_brk implements is the traditional one: it returns the new
// break, or the unchanged old break if the request could not be satisfied. So
// "did it work" is "did the value move", which is what the comparison below
// tests. brk(0) is the query form.

void *_sbrk(ptrdiff_t incr)
{
    long current = __sys1(SYS_brk, 0);
    if (current < 0) { errno = ENOMEM; return (void *)-1; }

    if (incr == 0) { return (void *)current; }

    long requested = current + (long)incr;

    // A negative incr large enough to wrap below zero would otherwise be sent
    // to the kernel as a huge positive address.
    if (incr < 0 && requested > current) { errno = ENOMEM; return (void *)-1; }
    if (incr > 0 && requested < current) { errno = ENOMEM; return (void *)-1; }

    long result = __sys1(SYS_brk, requested);
    if (result != requested) { errno = ENOMEM; return (void *)-1; }

    // sbrk returns the *previous* break - the base of the newly added space.
    return (void *)current;
}

// ---------------- process ----------------

void _exit(int status)
{
    (void)__sys1(SYS_exit, status);

    // SYS_exit does not return - it records the status and traps to
    // EXIT_VECTOR, which never comes back. But _exit is declared noreturn, so
    // give the compiler something that visibly does not fall through.
    for (;;) { }
}

int _getpid(void)
{
    long rc = __sys0(SYS_getpid);
    // SYS_getpid is still a stub. Report something valid rather than -1;
    // nothing here depends on it being a real pid.
    return rc < 0 ? 1 : (int)rc;
}

int _kill(int pid, int sig)
{
    (void)pid; (void)sig;
    errno = ENOSYS;   // no signals
    return -1;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv == 0) { errno = EFAULT; return -1; }

    long ms = __sys0(SYS_gettime);
    if (ms < 0) { errno = ENOSYS; return -1; }   // SYS_gettime still a stub

    tv->tv_sec  = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}

clock_t _times(struct tms *buf)
{
    (void)buf;
    errno = ENOSYS;
    return (clock_t)-1;
}

// ---------------- sleeping ----------------
//
// newlib declares sleep() and usleep() in sys/unistd.h and nanosleep() in
// time.h, and defines none of them: they are all considered the target's job.
// SYS_sleep is the target's answer.
//
// Two properties of the kernel side leak through all three, and no amount of
// wrapping here can hide them:
//
//   - The timer runs at 100 Hz (TIMER_MS_PER_TICK is 10), and the kernel
//     rounds a request up to a whole tick and wakes sleepers from the timer
//     interrupt. So a sleep is never shorter than asked for, and may overshoot
//     by up to one tick on top of whatever the rounding added. usleep(1) and
//     usleep(9999) both sleep ten milliseconds.
//   - There are no signals, so nothing can interrupt a sleep. These never
//     return EINTR, and nanosleep()'s remaining-time argument is always zero
//     on return.
//
// Everything funnels through one place so the rounding rule is stated once.
// Rounding sub-millisecond requests up to 1 rather than down to 0 keeps the
// guarantee that a non-zero request always yields the CPU at least once; a
// caller that wants a pure yield asks for zero.
static void secureos_sleep_ms(unsigned long ms)
{
    if (ms == 0) { return; }
    __sys1(SYS_sleep, ms);
}

unsigned sleep(unsigned int seconds)
{
    secureos_sleep_ms((unsigned long)seconds * 1000UL);

    // Seconds left unslept. Nothing can interrupt a sleep here, so always 0.
    return 0;
}

int usleep(useconds_t useconds)
{
    unsigned long ms = (unsigned long)useconds / 1000UL;

    if (useconds != 0 && ms == 0) {
        ms = 1;   // finer than the clock; round up rather than not sleep
    }

    secureos_sleep_ms(ms);
    return 0;
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    if (rqtp == 0) { errno = EFAULT; return -1; }

    if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec > 999999999L) {
        errno = EINVAL;
        return -1;
    }

    unsigned long ms = (unsigned long)rqtp->tv_sec * 1000UL +
                       (unsigned long)(rqtp->tv_nsec / 1000000L);

    if (ms == 0 && (rqtp->tv_sec != 0 || rqtp->tv_nsec != 0)) {
        ms = 1;
    }

    secureos_sleep_ms(ms);

    if (rmtp != 0) {
        rmtp->tv_sec  = 0;
        rmtp->tv_nsec = 0;
    }
    return 0;
}

// No filesystem unlink/link syscalls, and no fork/exec/wait at all.

int _unlink(const char *name)  { (void)name; errno = ENOSYS; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; errno = EMLINK; return -1; }
int _fork(void)                { errno = ENOSYS; return -1; }
int _wait(int *status)         { (void)status; errno = ECHILD; return -1; }
int _execve(const char *name, char * const *argv, char * const *env)
{
    (void)name; (void)argv; (void)env;
    errno = ENOMEM;
    return -1;
}

// ---------------- unprefixed aliases ----------------
//
// Built with --disable-newlib-supplied-syscalls, newlib's reentrant layer
// (newlib/libc/reent/*.c) calls the *unprefixed* names - _read_r() calls
// read(), _sbrk_r() calls sbrk(), and so on - while everything else in newlib
// and every convention for retargeting it uses the underscore-prefixed ones.
// Both spellings therefore have to exist, and they have to be the same
// function.
//
// Aliases rather than forwarding wrappers so there is exactly one
// implementation of each and no chance of the two drifting apart.
//
// Only the names newlib actually references are aliased; adding the rest
// would be dead weight that the linker cannot discard from a .o this small.

int     open(const char *name, int flags, ...)      __attribute__((alias("_open")));
int     close(int fd)                               __attribute__((alias("_close")));
_READ_WRITE_RETURN_TYPE read(int fd, void *buf, size_t len)        __attribute__((alias("_read")));
_READ_WRITE_RETURN_TYPE write(int fd, const void *buf, size_t len) __attribute__((alias("_write")));
_off_t  lseek(int fd, _off_t offset, int whence)    __attribute__((alias("_lseek")));
int     fstat(int fd, struct stat *st)              __attribute__((alias("_fstat")));
int     stat(const char *name, struct stat *st)     __attribute__((alias("_stat")));
int     isatty(int fd)                              __attribute__((alias("_isatty")));
int     getpid(void)                                __attribute__((alias("_getpid")));
int     kill(int pid, int sig)                      __attribute__((alias("_kill")));
int     unlink(const char *name)                    __attribute__((alias("_unlink")));
int     link(const char *a, const char *b)          __attribute__((alias("_link")));
void   *sbrk(ptrdiff_t incr)                        __attribute__((alias("_sbrk")));
int     gettimeofday(struct timeval *tv, void *tz)  __attribute__((alias("_gettimeofday")));
clock_t times(struct tms *buf)                      __attribute__((alias("_times")));

// __libc_init_array() calls _init() before running the .init_array
// constructors. It is the head of the classic .init section chain, which a
// hosted toolchain supplies in crti.o/crtn.o - files that only exist for a
// target with a full toolchain installed, which this is not.
//
// Nothing here uses .init: user.ld collects constructors into .init_array,
// which __libc_init_array walks directly. So _init exists to be called and to
// return, and _fini is its counterpart for the exit path.
void _init(void) { }
void _fini(void) { }
