#ifndef PI_CLIPBOARD_IO_H
#define PI_CLIPBOARD_IO_H

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>

#define MAX_CLIPBOARD_BYTES (50u * 1024u * 1024u)
#define CLIPBOARD_TIMEOUT_MS 2000

static int64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// Reuse the operation's deadline: neither incoming chunks nor signals extend it.
static int wait_for_fd(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) return 0;
        struct pollfd descriptor = {fd, events, 0};
        int ready = poll(&descriptor, 1, (int)remaining);
        if (ready >= 0) return ready ? descriptor.revents : 0;
        if (errno != EINTR) return 0;
    }
}

#endif
