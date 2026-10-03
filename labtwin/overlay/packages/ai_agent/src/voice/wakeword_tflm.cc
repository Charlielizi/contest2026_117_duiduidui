#include "voice/wakeword_engine.h"
#include "voice/wakeword_model_data.h"
#include "voice/wakeword_labels.h"

#include <errno.h>
#include <cmath>
#include <stdint.h>
#include <string.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

namespace {

constexpr size_t kWindowSamples = 24000;  // 1.5 s at 16 kHz
constexpr size_t kArenaBytes = 256 * 1024;
constexpr size_t kFeatureFrames = 148;
constexpr size_t kFeatureBins = 32;
alignas(16) uint8_t g_arena[kArenaBytes];
int16_t g_window[kWindowSamples];
size_t g_window_fill;
uint16_t g_features[kFeatureFrames * kFeatureBins];
size_t g_feature_fill;
FrontendState g_frontend;
bool g_frontend_ready;
bool g_use_microfrontend;

const tflite::Model *g_model;
using Resolver = tflite::MicroMutableOpResolver<9>;
Resolver *g_resolver;
tflite::MicroInterpreter *g_interpreter;
TfLiteTensor *g_input;
TfLiteTensor *g_output;

int backend_init()
{
    if (g_openvela_wakeword_model_len < 16) return -ENOENT;
    g_model = tflite::GetModel(g_openvela_wakeword_model);
    if (!g_model || g_model->version() != TFLITE_SCHEMA_VERSION)
        return -EINVAL;

    g_use_microfrontend = wakeword_uses_microfrontend(
        g_openvela_wakeword_model_version);
    static Resolver resolver;
    resolver.AddReshape();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddAveragePool2D();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();
    resolver.AddQuantize();
    resolver.AddDequantize();
    resolver.AddMean();
    g_resolver = &resolver;

    static tflite::MicroInterpreter interpreter(
        g_model, resolver, g_arena, kArenaBytes);
    g_interpreter = &interpreter;
    if (g_interpreter->AllocateTensors() != kTfLiteOk) return -ENOMEM;
    g_input = g_interpreter->input(0);
    g_output = g_interpreter->output(0);
    if (!g_input || !g_output || g_input->type != kTfLiteInt8 ||
        g_output->type != kTfLiteInt8 ||
        g_input->bytes != (g_use_microfrontend ?
                          kFeatureFrames * kFeatureBins : kWindowSamples) ||
        !wakeword_labels_valid(g_output->bytes, g_openvela_wakeword_model_version) ||
        !std::isfinite(g_input->params.scale) || g_input->params.scale <= 0 ||
        !std::isfinite(g_output->params.scale) || g_output->params.scale <= 0)
        return -EINVAL;
    if (g_use_microfrontend) {
        FrontendConfig config;
        FrontendFillConfigWithDefaults(&config);
        config.window.size_ms = 25;
        config.window.step_size_ms = 10;
        config.filterbank.num_channels = kFeatureBins;
        if (!FrontendPopulateState(&config, &g_frontend, 16000)) {
            FrontendFreeStateContents(&g_frontend);
            return -ENOMEM;
        }
        g_frontend_ready = true;
    }
    g_window_fill = 0;
    g_feature_fill = 0;
    memset(g_window, 0, sizeof(g_window));
    return 0;
}

int backend_process(const int16_t *pcm, size_t samples,
                    wakeword_language_t *language, float *score)
{
    if (!g_interpreter || !pcm || !language || !score) return -EINVAL;
    if (g_use_microfrontend) {
        size_t offset = 0;
        while (offset < samples) {
            size_t used = 0;
            FrontendOutput output = FrontendProcessSamples(
                &g_frontend, pcm + offset, samples - offset, &used);
            if (!used && !output.size) break;
            offset += used;
            if (output.size == kFeatureBins) {
                if (g_feature_fill == kFeatureFrames) {
                    memmove(g_features, g_features + kFeatureBins,
                            (kFeatureFrames - 1) * kFeatureBins * sizeof(uint16_t));
                    g_feature_fill--;
                }
                memcpy(g_features + g_feature_fill * kFeatureBins,
                       output.values, kFeatureBins * sizeof(uint16_t));
                g_feature_fill++;
            }
        }
        if (g_feature_fill < kFeatureFrames) return 0;
        const float input_scale = g_input->params.scale;
        const int input_zero = g_input->params.zero_point;
        for (size_t i = 0; i < kFeatureFrames * kFeatureBins; i++) {
            float value = roundf((g_features[i] / 25.6f) / input_scale) + input_zero;
            if (value < -128) value = -128;
            if (value > 127) value = 127;
            g_input->data.int8[i] = (int8_t)value;
        }
    } else if (samples >= kWindowSamples) {
        memcpy(g_window, pcm + samples - kWindowSamples, sizeof(g_window));
        g_window_fill = kWindowSamples;
    } else {
        memmove(g_window, g_window + samples,
                (kWindowSamples - samples) * sizeof(int16_t));
        memcpy(g_window + kWindowSamples - samples, pcm,
               samples * sizeof(int16_t));
        if (g_window_fill < kWindowSamples) {
            g_window_fill += samples;
            if (g_window_fill > kWindowSamples) g_window_fill = kWindowSamples;
        }
    }
    if (!g_use_microfrontend) {
        if (g_window_fill < kWindowSamples) return 0;
        const float input_gain = 1.0f / (32768.0f * g_input->params.scale);
        for (size_t i = 0; i < kWindowSamples; i++) {
            float value = roundf(g_window[i] * input_gain) +
                          g_input->params.zero_point;
            if (value < -128) value = -128;
            if (value > 127) value = 127;
            g_input->data.int8[i] = (int8_t)value;
        }
    }
    if (g_interpreter->Invoke() != kTfLiteOk) return -EIO;

    const float scale = g_output->params.scale;
    const int zero = g_output->params.zero_point;
    int positive = wakeword_positive_index(g_output->data.int8, g_output->bytes);
    if (positive < 0) {
        *score = 0;
        return 0;
    }
    *language = g_output->bytes == 2 || positive == 0 ?
                WAKEWORD_LANG_ZH : WAKEWORD_LANG_EN;
    *score = (g_output->data.int8[positive] - zero) * scale;
    return 1;
}

void backend_reset()
{
    memset(g_window, 0, sizeof(g_window));
    g_window_fill = 0;
    memset(g_features, 0, sizeof(g_features));
    g_feature_fill = 0;
    if (g_frontend_ready) FrontendReset(&g_frontend);
}

void backend_deinit()
{
    if (g_frontend_ready) {
        FrontendFreeStateContents(&g_frontend);
        g_frontend_ready = false;
    }
    g_interpreter = nullptr;
    g_input = nullptr;
    g_output = nullptr;
}

const wakeword_engine_ops_t kOps = {
    "tflm-int8",
    g_openvela_wakeword_model_version,
    backend_init,
    backend_process,
    backend_reset,
    backend_deinit,
};

}  // namespace

extern "C" int wakeword_tflm_register(void)
{
    return wakeword_engine_register(&kOps);
}
