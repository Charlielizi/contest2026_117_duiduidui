#include "voice/wakeword_engine.h"
#include "voice/audio_capture.h"
#include "voice/audio_arbiter.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

struct audio_capture { int unused; };
audio_capture_t *audio_capture_open(const char *p, unsigned int r,
    unsigned int c, unsigned int b) { (void)p;(void)r;(void)c;(void)b; return NULL; }
int audio_capture_start(audio_capture_t *c) { (void)c; return -ENODEV; }
int audio_capture_read(audio_capture_t *c, void *b, size_t l)
{ (void)c;(void)b;(void)l; return -ENODEV; }
int audio_capture_abort(audio_capture_t *c) { (void)c; return 0; }
void audio_capture_close(audio_capture_t *c) { (void)c; }

static int detections;
static wakeword_detection_t last;
static int mock_init(void) { return 0; }
static int mock_process(const int16_t *p, size_t n,
    wakeword_language_t *l, float *s)
{ (void)p;(void)n;*l=WAKEWORD_LANG_ZH;*s=0.9f;return 1; }
static void detected(const wakeword_detection_t *d, void *arg)
{ (void)arg; last=*d; detections++; }

int main(void)
{
    static const wakeword_engine_ops_t ops = {
        "mock", "test-v1", mock_init, mock_process, NULL, NULL
    };
    assert(wakeword_engine_register(&ops) == 0);
    assert(wakeword_engine_init(detected, NULL) == 0);
    assert(wakeword_engine_test_detection(WAKEWORD_LANG_EN, 0.84f) == -ERANGE);
    assert(wakeword_engine_test_detection(WAKEWORD_LANG_EN, 0.91f) == 0);
    assert(detections == 1);
    assert(last.language == WAKEWORD_LANG_EN);
    assert(last.score > 0.90f);
    assert(audio_arbiter_acquire(AUDIO_OWNER_WAKEWORD) == 0);
    assert(audio_arbiter_acquire(AUDIO_OWNER_PTT) == -EBUSY);
    audio_arbiter_release(AUDIO_OWNER_WAKEWORD);
    puts("wakeword_host_test: PASS");
    return 0;
}
