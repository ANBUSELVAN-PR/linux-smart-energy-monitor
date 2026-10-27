// LD_PRELOAD shim for /dev/energy_monitor to feed deterministic test data
// Only intercepts open, poll, lseek, read for the specific device path.
// Other file operations are delegated to the real libc functions.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>

static const char *DEVICE_PATH = "/dev/energy_monitor";
static const int FAKE_FD = 100; // arbitrary unused fd number
static const char *scenario = NULL;
static const char *data_ptr = NULL;
static size_t data_len = 0;
static int read_done = 0;

static int is_fake_fd(int fd) {
    return fd == FAKE_FD;
}

static void init_scenario(void) __attribute__((constructor));
static void init_scenario(void) {
    // Read scenario from environment variable ENERGY_MONITOR_FAKE_DATA
    scenario = getenv("ENERGY_MONITOR_FAKE_DATA");
    if (!scenario) scenario = "valid"; // default

    if (strcmp(scenario, "valid") == 0) {
        data_ptr = "sample=1,voltage=230.0,current=5.0,power=1150.0,energy=100.0\n";
    } else if (strcmp(scenario, "incomplete") == 0) {
        data_ptr = "sample=2,voltage=230.0,current=5.0"; // missing fields, no newline
    } else if (strcmp(scenario, "malformed") == 0) {
        data_ptr = "sample=abc,voltage=xyz,current=5.0,power=,energy=100.0\n";
    } else if (strcmp(scenario, "trailing") == 0) {
        data_ptr = "sample=3,voltage=230.0,current=5.0,power=1150.0,energy=100.0XYZ\n";
    } else {
        // fallback to a simple valid record
        data_ptr = "sample=0,voltage=0.0,current=0.0,power=0.0,energy=0.0\n";
    }
    data_len = strlen(data_ptr);
    read_done = 0;
}

// ---------- open ----------
int open(const char *pathname, int flags, ...) {
    // Resolve the real variadic open once.
    static int (*real_open)(const char *, int, ...) = NULL;
    if (!real_open) {
        real_open = dlsym(RTLD_NEXT, "open");
    }
    if (strcmp(pathname, DEVICE_PATH) == 0) {
        // Return our fake descriptor; ignore any mode argument.
        return FAKE_FD;
    }
    // Forward the call, preserving optional mode argument.
    if (flags & O_CREAT) {
        va_list ap;
        mode_t mode;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        return real_open(pathname, flags, mode);
    }
    return real_open(pathname, flags);
}

// ---------- poll ----------
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    static int (*real_poll)(struct pollfd *, nfds_t, int) = NULL;
    if (!real_poll) {
        real_poll = dlsym(RTLD_NEXT, "poll");
    }
    for (nfds_t i = 0; i < nfds; ++i) {
        if (is_fake_fd(fds[i].fd)) {
            if (read_done) {
                // Stop cleanly after the application processes one record.
                raise(SIGINT);
                errno = EINTR;
                return -1;
            }

            fds[i].revents = POLLIN;
            return 1;
        }
    }
    return real_poll(fds, nfds, timeout);
}

// ---------- lseek ----------
off_t lseek(int fd, off_t offset, int whence) {
    static off_t (*real_lseek)(int, off_t, int) = NULL;
    if (!real_lseek) {
        real_lseek = dlsym(RTLD_NEXT, "lseek");
    }
    if (is_fake_fd(fd)) {
        // The program seeks to 0 before each read; succeed.
        return 0;
    }
    return real_lseek(fd, offset, whence);
}

// ---------- read ----------
ssize_t read(int fd, void *buf, size_t count) {
    static ssize_t (*real_read)(int, void *, size_t) = NULL;
    if (!real_read) {
        real_read = dlsym(RTLD_NEXT, "read");
    }
    if (is_fake_fd(fd)) {
        if (read_done) {
            // Simulate EOF after the first record.
            return 0;
        }
        size_t to_copy = data_len < count ? data_len : count;
        memcpy(buf, data_ptr, to_copy);
        read_done = 1;
        return (ssize_t)to_copy;
    }
    return real_read(fd, buf, count);
}

// ---------- close (optional) ----------
int close(int fd) {
    static int (*real_close)(int) = NULL;
    if (!real_close) {
        real_close = dlsym(RTLD_NEXT, "close");
    }
    if (is_fake_fd(fd)) {
        return 0;
    }
    return real_close(fd);
}
