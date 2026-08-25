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
// SYS_brk is still sys_stub (src/kernel/interrupts/syscall.c), so there is
// nothing behind a real sbrk yet. A static arena is enough for stdio's buffers
// and keeps this milestone honest: it isolates "does newlib work on this OS"
// from "does the kernel's break implementation work". Doom needs a real
// SYS_brk with a demand-paged anonymous reservation - that is a separate piece
// of work and this whole block goes away when it lands.
//
// The arena lives in .bss, so it costs nothing in the binary on disk, and it
// is demand-paged: only the pages actually touched get frames. crt0 zeroes it,
// which does mean every page is faulted in at startup.
#ifndef USER_HEAP_SIZE
#define USER_HEAP_SIZE (256 * 1024)
#endif

static char  __user_heap[USER_HEAP_SIZE];
static char *__user_brk = __user_heap;

void *_sbrk(ptrdiff_t incr)
{
    char *prev = __user_brk;

    if (incr < 0 && (prev + incr) < __user_heap) {
        errno = ENOMEM;
        return (void *)-1;
    }
    if (incr > 0 && (size_t)(prev - __user_heap) + (size_t)incr > USER_HEAP_SIZE) {
        errno = ENOMEM;
        return (void *)-1;
    }

    __user_brk = prev + incr;
    return prev;
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
