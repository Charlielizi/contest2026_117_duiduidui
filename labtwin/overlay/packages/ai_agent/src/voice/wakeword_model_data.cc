#include "voice/wakeword_model_data.h"

/* Replaced by tools/wakeword/package_model.py after training.  Keeping an
 * explicit invalid bootstrap asset makes an untrained firmware fail closed:
 * PTT remains available, but no audio is sent to the network automatically. */
const unsigned char g_openvela_wakeword_model[] = {0};
const size_t g_openvela_wakeword_model_len = 0;
const char g_openvela_wakeword_model_version[] = "untrained";
const char g_openvela_wakeword_model_sha256[] = "untrained";
