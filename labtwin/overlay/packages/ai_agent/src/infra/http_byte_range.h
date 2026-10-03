#ifndef AI_AGENT_HTTP_BYTE_RANGE_H
#define AI_AGENT_HTTP_BYTE_RANGE_H
#include <errno.h>
#include <stdlib.h>
#include <string.h>
/* Single byte ranges only. Multi-range responses are intentionally unsupported. */
static inline int http_byte_range(const char *range, unsigned long long size,
                                 unsigned long long *start, unsigned long long *end)
{
    char *next;
    unsigned long long value;
    *start = 0; *end = size ? size - 1 : 0;
    if (!size) return -ERANGE;
    if (!range) return 0;
    if (strncmp(range, "bytes=", 6) || strchr(range, ',')) return -EINVAL;
    const char *p = range + 6;
    if (*p == '-') {
        p++;
        if (*p < '0' || *p > '9') return -EINVAL;
        errno = 0; value = strtoull(p, &next, 10);
        if (errno || *next) return -EINVAL;
        if (!value) return -ERANGE;
        *start = value < size ? size - value : 0;
    } else {
        if (*p < '0' || *p > '9') return -EINVAL;
        errno = 0; value = strtoull(p, &next, 10);
        if (errno || *next != '-') return -EINVAL;
        *start = value; p = next + 1;
        if (*p) {
            if (*p < '0' || *p > '9') return -EINVAL;
            errno = 0; value = strtoull(p, &next, 10);
            if (errno || *next) return -EINVAL;
            if (value < *end) *end = value;
        }
    }
    return *start >= size || *end < *start ? -ERANGE : 1;
}
#endif
