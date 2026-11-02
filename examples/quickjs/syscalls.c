/* Bounded newlib support for the bare-metal guest. All heap addresses remain
 * within the browser host's 16 MiB RAM. Only stdout/stderr may be written. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

extern char _heap_start[], _heap_end[];
void *_sbrk(ptrdiff_t increment) {
    static uintptr_t current;
    if (!current) current = (uintptr_t)_heap_start;
    volatile const uint32_t *request = (volatile const uint32_t *)0xf000;
    const uintptr_t end = request[0] == 2 ? request[1] : (uintptr_t)_heap_end;
    if (increment < 0 || current > end || (uintptr_t)increment > end - current) {
        errno = ENOMEM;
        return (void *)-1;
    }
    void *result = (void *)current;
    current += (uintptr_t)increment;
    return result;
}
int _write(int fd, const void *buffer, size_t size) {
    if (fd != 1 && fd != 2) { errno = EBADF; return -1; }
    const unsigned char *bytes = buffer;
    for (size_t i = 0; i < size; i++) {
        register uint32_t value __asm__("r0") = bytes[i];
        __asm__ volatile("svc #1" :: "r"(value) : "memory");
    }
    return (int)size;
}
int _read(int fd, void *buffer, size_t size) { (void)fd; (void)buffer; (void)size; errno = EBADF; return -1; }
int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { return fd == 1 || fd == 2; }
off_t _lseek(int fd, off_t offset, int whence) { (void)fd; (void)offset; (void)whence; errno = ESPIPE; return -1; }
int _getpid(void) { return 1; }
int _kill(int pid, int signal) { (void)pid; (void)signal; errno = EINVAL; return -1; }
void _exit(int status) {
    register uint32_t value __asm__("r0") = (uint32_t)status;
    __asm__ volatile("svc #0" :: "r"(value) : "memory");
    for (;;) {}
}
int _gettimeofday(struct timeval *tv, void *tz) { (void)tz; tv->tv_sec = 0; tv->tv_usec = 0; return 0; }
