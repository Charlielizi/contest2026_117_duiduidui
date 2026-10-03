#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAKEWORD_LANG_ZH = 0,
    WAKEWORD_LANG_EN = 1
} wakeword_language_t;

typedef struct {
    wakeword_language_t language;
    float score;
    uint64_t monotonic_ms;
    const char *model_version;
} wakeword_detection_t;

typedef struct {
    uint64_t frames;
    uint64_t inferences;
    uint64_t detections;
    uint64_t capture_errors;
    uint32_t last_inference_us;
    uint32_t max_inference_us;
} wakeword_stats_t;

typedef struct wakeword_engine_ops {
    const char *name;
    const char *model_version;
    int (*init)(void);
    int (*process)(const int16_t *pcm, size_t samples,
                   wakeword_language_t *language, float *score);
    void (*reset)(void);
    void (*deinit)(void);
} wakeword_engine_ops_t;

typedef void (*wakeword_detected_cb_t)(const wakeword_detection_t *detection,
                                       void *arg);

int wakeword_engine_register(const wakeword_engine_ops_t *ops);
int wakeword_engine_init(wakeword_detected_cb_t callback, void *arg);
int wakeword_engine_start(void);
int wakeword_engine_pause(void);
int wakeword_engine_resume(void);
bool wakeword_engine_is_listening(void);
void wakeword_engine_stop(void);
void wakeword_engine_get_stats(wakeword_stats_t *stats);
const char *wakeword_engine_backend(void);

/* Test hook: inject a backend result without opening an audio device. */
int wakeword_engine_test_detection(wakeword_language_t language, float score);

#ifdef __cplusplus
}
#endif
