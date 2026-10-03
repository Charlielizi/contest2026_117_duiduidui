#include <math.h>

/* This NuttX configuration omits log1pf while the fixed-point frontend uses it
 * only during one-time filterbank initialization. Keep the exact formula. */
extern "C" __attribute__((weak)) float log1pf(float value)
{
    return logf(1.0f + value);
}

/* TFLM strips micro_log.cc with TF_LITE_STRIP_ERROR_STRINGS, but this pinned
 * resolver revision still retains references from error-only branches. */
__attribute__((weak)) void MicroPrintf(const char *format, ...)
{
    (void)format;
}
