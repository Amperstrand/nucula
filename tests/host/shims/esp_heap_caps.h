// Host shim: the load-path heap preflight asks for the largest free
// block; on the host that is effectively unbounded, so loads proceed.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_DEFAULT 0

static inline size_t heap_caps_get_largest_free_block(int caps)
{
    (void)caps;
    return 64u * 1024u * 1024u;
}

#ifdef __cplusplus
}
#endif
