/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize voice channel (load config keys). */
int voice_channel_init(void);

/* Start voice recording, ASR, then push text to agent via message_bus. */
int voice_channel_start(void);

/* Start a wake-word initiated command. Recording stops after max_ms or after
 * silence_ms of silence following detected speech. */
int voice_channel_start_auto(unsigned int max_ms, unsigned int silence_ms);
int voice_channel_auto_done(void);

/* Check cloud-ASR readiness without taking ownership of the microphone. */
int voice_channel_is_ready(void);

/* Stop an active voice session. */
int voice_channel_stop(void);

/* Abort recording and release the microphone without waiting for ASR/LLM. */
int voice_channel_cancel(void);

/* Stop recording and return ASR text to caller (does NOT push inbound).
 * text_out: buffer to receive ASR text, text_cap: buffer capacity.
 * Returns 0 on success, negative errno on failure.
 * If ASR returns empty text, text_out[0] is set to '\0' (not an error). */
int voice_channel_stop_with_text(char *text_out, size_t text_cap);

/* Synthesize text and play back (called from outbound dispatcher). */
int voice_channel_speak(const char *text);

/* Test TTS: synthesize text, save PCM to file. */
int voice_channel_test_tts(const char *text, const char *out_path);

/* Test ASR: read PCM file, recognize, print result. */
int voice_channel_test_asr(const char *pcm_path);

#ifdef __cplusplus
}
#endif
