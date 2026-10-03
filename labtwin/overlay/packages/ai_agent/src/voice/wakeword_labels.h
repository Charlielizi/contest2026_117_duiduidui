#ifndef WAKEWORD_LABELS_H
#define WAKEWORD_LABELS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Four-class legacy: zh,en,unknown,silence. Two-class: unknown,zh.
 * A version prefix explicitly opts into the binary contract. */
static inline int wakeword_labels_valid(size_t count, const char *version)
{
    int binary = version && (strncmp(version, "zh2-v1:", 7) == 0 ||
                             strncmp(version, "zh2-mf1:", 8) == 0);
    return binary ? count == 2 : count == 4;
}

static inline int wakeword_uses_microfrontend(const char *version)
{
    return version && strncmp(version, "zh2-mf1:", 8) == 0;
}

/* Return a positive class index, or -1 for background/ties. */
static inline int wakeword_positive_index(const int8_t *scores, size_t count)
{
    if (!scores) return -1;
    if (count == 2) return scores[1] > scores[0] ? 1 : -1;
    if (count != 4) return -1;
    int best = scores[0] >= scores[1] ? 0 : 1;
    return scores[best] > scores[2] && scores[best] > scores[3] ? best : -1;
}
#endif
