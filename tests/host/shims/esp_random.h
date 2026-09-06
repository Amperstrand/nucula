#pragma once
// Host shim: RNG via getrandom(2).
#include <stddef.h>
#include <sys/random.h>

static inline void esp_fill_random(unsigned char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom(buf + off, len - off, 0);
        if (n > 0) off += (size_t)n;
    }
}
