#include "lab_ui_controller.h"
#include "lab_ui_flow.h"
#include "weather_service.h"
#include "device_settings.h"
#include "infra/admin_auth.h"
#include "labtwin/labtwin.h"
#include "voice/audio_arbiter.h"
#include "voice/audio_playback.h"
#ifdef CONFIG_AI_AGENT_WAKEWORD
#include "voice/voice_action.h"
#include "voice/wakeword_session.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;
    bool stop;
    bool pending;
} alert_audio_state_t;

static lab_ui_t *g_ui;
static lv_timer_t *g_refresh_timer;
static lab_ui_flow_t g_flow;
static char g_environment_event_id[LABTWIN_ENV_EVENT_ID_MAX];
static char g_last_alert_tone_id[LABTWIN_ENV_EVENT_ID_MAX];
static uint64_t g_last_alert_tone_ms;
static unsigned int g_alert_tone_count;
static lab_ui_controller_settings_cb_t g_settings_cb;
static void *g_settings_arg;
static weather_service_result_t g_weather;
static bool g_weather_valid;
static bool g_service_ready;
static unsigned int g_service_retry_ticks;
static bool g_new_experiment_voice;
/* An expired timer remains persisted.  Remember the displayed experiment so
 * dismissing its alert does not reopen it every refresh tick. */
static char g_timer_expiry_experiment_id[32];
#ifdef CONFIG_AI_AGENT_WAKEWORD
static bool g_voice_dismissed;
static int g_voice_start_error;
#endif
static alert_audio_state_t g_alert_audio = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static uint64_t controller_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int play_alert_tone(void)
{
    enum { RATE = 16000, SAMPLES = 3200 };
    int16_t *pcm;
    audio_playback_t *playback;
    int i;
    int ret = -1;
    if (audio_arbiter_acquire(AUDIO_OWNER_ALERT) != 0) return -1;
    pcm = malloc(SAMPLES * sizeof(*pcm));
    if (!pcm) goto done;
    for (i = 0; i < SAMPLES; i++) {
        int phase = i % 32;
        pcm[i] = (i > 1200 && i < 1600) ? 0 :
                 (phase < 16 ? 7000 : -7000);
    }
    playback = audio_playback_open("/dev/audio/pcm0p", RATE, 1, 16);
    if (playback) {
        ret = audio_playback_write(playback, pcm, SAMPLES * sizeof(*pcm));
        audio_playback_close(playback);
    }
    free(pcm);
done:
    audio_arbiter_release(AUDIO_OWNER_ALERT);
    return ret < 0 ? -1 : 0;
}

static void *alert_audio_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_alert_audio.lock);
    while (!g_alert_audio.stop) {
        while (!g_alert_audio.pending && !g_alert_audio.stop)
            pthread_cond_wait(&g_alert_audio.cond, &g_alert_audio.lock);
        if (g_alert_audio.stop) break;
        g_alert_audio.pending = false;
        pthread_mutex_unlock(&g_alert_audio.lock);
        play_alert_tone();
        pthread_mutex_lock(&g_alert_audio.lock);
    }
    pthread_mutex_unlock(&g_alert_audio.lock);
    return NULL;
}

static int alert_audio_start(void)
{
    int ret;
    pthread_mutex_lock(&g_alert_audio.lock);
    if (g_alert_audio.running) {
        pthread_mutex_unlock(&g_alert_audio.lock);
        return 0;
    }
    g_alert_audio.stop = false;
    g_alert_audio.pending = false;
    pthread_mutex_unlock(&g_alert_audio.lock);
    ret = pthread_create(&g_alert_audio.thread, NULL, alert_audio_worker, NULL);
    if (ret != 0) return -1;
    pthread_mutex_lock(&g_alert_audio.lock);
    g_alert_audio.running = true;
    pthread_mutex_unlock(&g_alert_audio.lock);
    return 0;
}

static void alert_audio_stop(void)
{
    pthread_t thread;
    bool running;
    pthread_mutex_lock(&g_alert_audio.lock);
    running = g_alert_audio.running;
    thread = g_alert_audio.thread;
    g_alert_audio.stop = true;
    pthread_cond_signal(&g_alert_audio.cond);
    pthread_mutex_unlock(&g_alert_audio.lock);
    if (!running) return;
    pthread_join(thread, NULL);
    pthread_mutex_lock(&g_alert_audio.lock);
    g_alert_audio.running = false;
    g_alert_audio.pending = false;
    pthread_mutex_unlock(&g_alert_audio.lock);
}

static void maybe_queue_alert(const char *event_id)
{
    uint64_t now = controller_monotonic_ms();
    if (!event_id || !event_id[0]) return;
    if (strcmp(g_last_alert_tone_id, event_id) != 0) {
        snprintf(g_last_alert_tone_id, sizeof(g_last_alert_tone_id), "%s",
                 event_id);
        g_last_alert_tone_ms = 0;
        g_alert_tone_count = 0;
    }
    if (g_alert_tone_count >= 5 ||
        (g_last_alert_tone_ms && now - g_last_alert_tone_ms < 30000ULL))
        return;
    pthread_mutex_lock(&g_alert_audio.lock);
    if (g_alert_audio.running) {
        g_alert_audio.pending = true;
        pthread_cond_signal(&g_alert_audio.cond);
        g_last_alert_tone_ms = now;
        g_alert_tone_count++;
    }
    pthread_mutex_unlock(&g_alert_audio.lock);
}

static bool active_state(labtwin_state_t state)
{
    return state == LABTWIN_STATE_RUNNING ||
           state == LABTWIN_STATE_PAUSED ||
           state == LABTWIN_STATE_RECOVERY_ERROR;
}

static const char *state_text(labtwin_state_t state)
{
    switch (state) {
    case LABTWIN_STATE_READY: return "待开始";
    case LABTWIN_STATE_RUNNING: return "运行中";
    case LABTWIN_STATE_PAUSED: return "已暂停";
    case LABTWIN_STATE_COMPLETED: return "已完成";
    case LABTWIN_STATE_CANCELLED: return "已取消";
    default: return "需恢复";
    }
}

static const char *sensor_unit(const char *sensor)
{
    if (sensor && strstr(sensor, "temperature")) return "℃";
    if (sensor && strstr(sensor, "humidity")) return "%";
    if (sensor && strstr(sensor, "proximity")) return "厘米";
    return "";
}

static void format_relative_time(char *output, size_t output_size,
                                 int64_t updated_epoch,
                                 labtwin_state_t state)
{
    int64_t delta;
    if (!labtwin_clock_trusted() || updated_epoch <= 0) {
        snprintf(output, output_size, "%s", state_text(state));
        return;
    }
    delta = (int64_t)time(NULL) - updated_epoch;
    if (delta < 0) delta = 0;
    if (delta < 60) snprintf(output, output_size, "刚刚");
    else if (delta < 3600) snprintf(output, output_size, "%ld分钟前",
                                    (long)(delta / 60));
    else if (delta < 86400) snprintf(output, output_size, "%ld小时前",
                                     (long)(delta / 3600));
    else snprintf(output, output_size, "%ld天前", (long)(delta / 86400));
}

static void format_environment(char *output, size_t output_size,
                               const labtwin_sensor_snapshot_t *sensors)
{
    char temperature[16];
    char humidity[16];
    char proximity[16];
    if (sensors->temperature_available)
        snprintf(temperature, sizeof(temperature), "%.1f",
                 sensors->temperature_c);
    else
        snprintf(temperature, sizeof(temperature), "--");
    if (sensors->humidity_available)
        snprintf(humidity, sizeof(humidity), "%.0f",
                 sensors->humidity_percent);
    else
        snprintf(humidity, sizeof(humidity), "--");
    if (sensors->proximity_available)
        snprintf(proximity, sizeof(proximity), "%.1f", sensors->proximity_cm);
    else
        snprintf(proximity, sizeof(proximity), "--");
    snprintf(output, output_size, "温度 %s℃  湿度 %s%%  距离 %s厘米%s",
             temperature, humidity, proximity,
             g_service_ready && labtwin_environment_has_storage_error() ?
               "  存储故障" : "");
}

static int get_active_view(labtwin_experiment_view_t *view)
{
    labtwin_experiment_summary_t items[LABTWIN_MAX_EXPERIMENTS];
    size_t count = 0;
    size_t i;
    if (labtwin_get_focused_view(view) == 0 && active_state(view->state))
        return 0;
    if (labtwin_experiment_list_summaries(items, LABTWIN_MAX_EXPERIMENTS,
                                          &count) != 0)
        return -1;
    for (i = 0; i < count; i++) {
        if (!active_state(items[i].state)) continue;
        if (labtwin_focus_experiment(items[i].experiment_id) == 0)
            return labtwin_get_focused_view(view);
    }
    memset(view, 0, sizeof(*view));
    return -1;
}

static void fill_experiment_lists(lab_ui_snapshot_t *snapshot,
                                  const labtwin_experiment_view_t *focused,
                                  bool have_view)
{
    labtwin_experiment_summary_t items[LABTWIN_MAX_EXPERIMENTS];
    size_t count = 0;
    size_t i;
    int active_slot = 0;
    int recent_slot = 0;
    int timer_slot = 0;
    if (labtwin_experiment_list_summaries(items, LABTWIN_MAX_EXPERIMENTS,
                                          &count) != 0)
        return;

    for (i = 0; i < count && active_slot < LAB_UI_EXPERIMENT_COUNT; i++) {
        lab_ui_experiment_item_t *item;
        if (items[i].state != LABTWIN_STATE_READY &&
            !active_state(items[i].state)) continue;
        item = &snapshot->experiments[active_slot++];
        snprintf(item->experiment_id, sizeof(item->experiment_id), "%s",
                 items[i].experiment_id);
        snprintf(item->name, sizeof(item->name), "%s", items[i].name);
        snprintf(item->meta, sizeof(item->meta), "%s · 步骤 %u/%u",
                 state_text(items[i].state), items[i].current_step + 1,
                 items[i].step_count ? items[i].step_count : 1);
        item->ready = items[i].state == LABTWIN_STATE_READY;
        item->paused = items[i].state == LABTWIN_STATE_PAUSED;
    }
    snapshot->experiment_count = (uint8_t)active_slot;

    for (i = 0; i < count && recent_slot < LAB_UI_RECENT_COUNT; i++) {
        lab_ui_experiment_item_t *item;
        if (items[i].state != LABTWIN_STATE_COMPLETED &&
            items[i].state != LABTWIN_STATE_CANCELLED) continue;
        item = &snapshot->recent[recent_slot++];
        snprintf(item->experiment_id, sizeof(item->experiment_id), "%s",
                 items[i].experiment_id);
        snprintf(item->name, sizeof(item->name), "%s", items[i].name);
        format_relative_time(item->meta, sizeof(item->meta),
                             items[i].updated_epoch, items[i].state);
    }
    snapshot->recent_count = (uint8_t)recent_slot;

    if (have_view && focused) {
        for (i = 0; i < count; i++) {
            if (strcmp(items[i].experiment_id, focused->experiment_id) != 0)
                continue;
            snprintf(snapshot->timers[0].experiment,
                     sizeof(snapshot->timers[0].experiment), "%s",
                     items[i].experiment_id);
            snprintf(snapshot->timers[0].task,
                     sizeof(snapshot->timers[0].task), "%s", items[i].name);
            snapshot->timers[0].remaining_seconds =
                items[i].has_timer ? items[i].primary_remaining_seconds : 0;
            snapshot->timers[0].expired = items[i].timer_expired;
            timer_slot = 1;
            break;
        }
    }
    for (i = 0; i < count && timer_slot < LAB_UI_TIMER_COUNT; i++) {
        if (!active_state(items[i].state) ||
            (have_view && focused &&
             strcmp(items[i].experiment_id, focused->experiment_id) == 0))
            continue;
        snprintf(snapshot->timers[timer_slot].experiment,
                 sizeof(snapshot->timers[timer_slot].experiment), "%s",
                 items[i].experiment_id);
        snprintf(snapshot->timers[timer_slot].task,
                 sizeof(snapshot->timers[timer_slot].task), "%s", items[i].name);
        snapshot->timers[timer_slot].remaining_seconds =
            items[i].has_timer ? items[i].primary_remaining_seconds : 0;
        snapshot->timers[timer_slot].expired = items[i].timer_expired;
        timer_slot++;
    }
}

static int find_active_expired_timer(lab_ui_timer_item_t *timer)
{
    labtwin_experiment_summary_t items[LABTWIN_MAX_EXPERIMENTS];
    size_t count = 0;
    size_t i;

    if (!timer ||
        labtwin_experiment_list_summaries(items, LABTWIN_MAX_EXPERIMENTS,
                                          &count) != 0)
        return -1;
    for (i = 0; i < count; i++) {
        if (!active_state(items[i].state) || !items[i].timer_expired)
            continue;
        memset(timer, 0, sizeof(*timer));
        snprintf(timer->experiment, sizeof(timer->experiment), "%s",
                 items[i].experiment_id);
        snprintf(timer->task, sizeof(timer->task), "%s", items[i].name);
        timer->expired = true;
        return 0;
    }
    return -1;
}

static void fill_base_snapshot(lab_ui_snapshot_t *snapshot)
{
    labtwin_sensor_snapshot_t sensors;
    device_wifi_status_t wifi_status;
    weather_service_result_t weather;
    bool clock_synced;
    time_t now = time(NULL) + 8 * 3600;
    struct tm tm_value;
    memset(snapshot, 0, sizeof(*snapshot));
    if (weather_service_poll(&weather)) {
        g_weather = weather;
        g_weather_valid = true;
    }
    if (g_weather_valid) {
        snprintf(snapshot->weather, sizeof(snapshot->weather), "%s%s",
                 g_weather.from_ip_location ? "" : "东莞·", g_weather.text);
        snprintf(snapshot->weather_temperature,
                 sizeof(snapshot->weather_temperature), "%s",
                 g_weather.temperature);
        snapshot->weather_code = (uint8_t)g_weather.weather_code;
        snapshot->weather_location_fallback = !g_weather.from_ip_location;
    }
    memset(&wifi_status, 0, sizeof(wifi_status));
    snapshot->wifi_connected =
        device_wifi_get_status(&wifi_status) == 0 && wifi_status.ip[0] &&
        strcmp(wifi_status.ip, "0.0.0.0") != 0;
    clock_synced = weather_service_clock_synced();
    if (clock_synced) {
        gmtime_r(&now, &tm_value);
        snprintf(snapshot->clock, sizeof(snapshot->clock), "%02d:%02d",
                 tm_value.tm_hour, tm_value.tm_min);
        snprintf(snapshot->date, sizeof(snapshot->date),
                 "%04d.%02d.%02d · UTC+8", tm_value.tm_year + 1900,
                 tm_value.tm_mon + 1, tm_value.tm_mday);
    } else {
        snprintf(snapshot->clock, sizeof(snapshot->clock), "--:--");
        snprintf(snapshot->date, sizeof(snapshot->date), "%s",
                 snapshot->wifi_connected ? "已联网 · 正在自动校时" :
                                            "未联网 · 请在设置中连接 Wi-Fi");
    }
    labtwin_sensor_get(&sensors);
    format_environment(snapshot->environment, sizeof(snapshot->environment),
                       &sensors);
    if (sensors.temperature_available)
        snprintf(snapshot->temperature, sizeof(snapshot->temperature), "%.1f℃",
                 sensors.temperature_c);
    else if (g_weather_valid)
        snprintf(snapshot->temperature, sizeof(snapshot->temperature), "外%s",
                 g_weather.temperature);
    else
        snprintf(snapshot->temperature, sizeof(snapshot->temperature), "未检测");
    if (sensors.humidity_available)
        snprintf(snapshot->humidity, sizeof(snapshot->humidity), "%.0f%%",
                 sensors.humidity_percent);
    else if (g_weather_valid)
        snprintf(snapshot->humidity, sizeof(snapshot->humidity), "外%s",
                 g_weather.humidity);
    else
        snprintf(snapshot->humidity, sizeof(snapshot->humidity), "未检测");
    if (sensors.proximity_available)
        snprintf(snapshot->distance, sizeof(snapshot->distance), "%.1f厘米",
                 sensors.proximity_cm);
    else
        snprintf(snapshot->distance, sizeof(snapshot->distance), "未检测");
}

static void fill_view_snapshot(lab_ui_snapshot_t *snapshot,
                               const labtwin_experiment_view_t *view)
{
    int i;
    snprintf(snapshot->experiment, sizeof(snapshot->experiment), "%s %s",
             view->experiment_id, view->name);
    snprintf(snapshot->step, sizeof(snapshot->step), "%s", view->step);
    snapshot->step_index = view->step_index + 1;
    snapshot->step_count = view->step_count;
    snapshot->paused = view->state == LABTWIN_STATE_PAUSED;
    snapshot->remaining_seconds = 0;
    for (i = 0; i < view->timer_count; i++) {
        if (view->timers[i].state == LABTWIN_TIMER_CANCELLED) continue;
        snapshot->remaining_seconds =
            view->timers[i].state == LABTWIN_TIMER_EXPIRED ? -1 :
            (int32_t)view->timers[i].remaining_seconds;
        break;
    }
}

static void fill_alert_snapshot(lab_ui_snapshot_t *snapshot,
                                lab_ui_alert_kind_t kind,
                                const labtwin_environment_event_view_t *event)
{
    const char *unit;
    if (kind == LAB_UI_ALERT_ENVIRONMENT && event) {
        unit = sensor_unit(event->sensor);
        snprintf(snapshot->alert_title, sizeof(snapshot->alert_title),
                 "%s 环境告警", event->rule_id);
        snprintf(snapshot->alert_id, sizeof(snapshot->alert_id), "%s · %s",
                 event->event_id, event->rule_id);
        snprintf(snapshot->alert_current, sizeof(snapshot->alert_current),
                 "%.1f%s", event->measured_value, unit);
        snprintf(snapshot->alert_threshold, sizeof(snapshot->alert_threshold),
                 "%.1f%s", event->trigger_threshold, unit);
        snprintf(snapshot->alert_detail, sizeof(snapshot->alert_detail),
                 "%s%s%s", event->experiment_id[0] ? "实验 " : "",
                 event->experiment_id,
                 event->experiment_id[0] ? "；事件已写入时间线" :
                                           "事件已写入时间线");
        snapshot->alert_confirm_only = true;
    } else if (kind == LAB_UI_ALERT_RECOVERY_NOTICE && event) {
        snprintf(snapshot->alert_title, sizeof(snapshot->alert_title),
                 "环境已恢复");
        snprintf(snapshot->alert_id, sizeof(snapshot->alert_id), "%s · %s",
                 event->event_id, event->rule_id);
        snprintf(snapshot->alert_detail, sizeof(snapshot->alert_detail),
                 "%s 已回到恢复范围", event->rule_id);
        snapshot->alert_no_actions = true;
    } else if (kind == LAB_UI_ALERT_TIME_RECOVERY) {
        snprintf(snapshot->alert_title, sizeof(snapshot->alert_title),
                 "计时已安全暂停");
        snprintf(snapshot->alert_detail, sizeof(snapshot->alert_detail),
                 "系统时间不可信。确认后从最后剩余时间继续。");
    } else if (kind == LAB_UI_ALERT_TIMER_EXPIRED) {
        snprintf(snapshot->alert_title, sizeof(snapshot->alert_title),
                 "计时已到期");
        snprintf(snapshot->alert_id, sizeof(snapshot->alert_id), "%s",
                 g_timer_expiry_experiment_id[0] ?
                 g_timer_expiry_experiment_id : "实验计时");
        snprintf(snapshot->alert_detail, sizeof(snapshot->alert_detail),
                 "倒计时已结束，结果已写入实验记录。确认后返回对话。");
        snprintf(snapshot->alert_current, sizeof(snapshot->alert_current),
                 "00:00");
        snapshot->alert_confirm_only = true;
    } else if (kind == LAB_UI_ALERT_COMPLETE) {
        snprintf(snapshot->alert_title, sizeof(snapshot->alert_title),
                 "确认完成实验");
        snprintf(snapshot->alert_detail, sizeof(snapshot->alert_detail),
                 "最后一步将标记完成，未结束计时器将停止。");
    } else if (kind == LAB_UI_ALERT_CANCEL) {
        snprintf(snapshot->alert_title, sizeof(snapshot->alert_title),
                 "确认终止实验");
        snprintf(snapshot->alert_detail, sizeof(snapshot->alert_detail),
                 "实验将记录为已取消，未完成计时器将停止。");
    }
}

static void refresh(lv_timer_t *timer)
{
    lab_ui_snapshot_t snapshot;
    labtwin_experiment_view_t view;
    labtwin_environment_event_view_t environment_event;
    bool have_view;
    int service_ret;
    (void)timer;
    if (!g_ui) return;
    fill_base_snapshot(&snapshot);

    if (!g_service_ready && ++g_service_retry_ticks >= 5) {
        g_service_retry_ticks = 0;
        service_ret = labtwin_service_init();
        if (service_ret == 0) g_service_ready = true;
    }
    have_view = g_service_ready && get_active_view(&view) == 0;
    fill_experiment_lists(&snapshot, have_view ? &view : NULL, have_view);
    lab_ui_flow_sync_content(&g_flow, have_view);
    labtwin_environment_set_foreground(have_view ? view.experiment_id : NULL);

    if (g_service_ready &&
        labtwin_environment_get_active(&environment_event) == 0) {
        if (g_flow.alert_kind != LAB_UI_ALERT_ENVIRONMENT)
            lab_ui_flow_show_alert(&g_flow, LAB_UI_ALERT_ENVIRONMENT);
        snprintf(g_environment_event_id, sizeof(g_environment_event_id), "%s",
                 environment_event.event_id);
        fill_alert_snapshot(&snapshot, LAB_UI_ALERT_ENVIRONMENT,
                            &environment_event);
        maybe_queue_alert(environment_event.event_id);
        snapshot.page = g_flow.page;
        snapshot.experiment_picker_open = false;
        lab_ui_set_snapshot(g_ui, &snapshot);
        return;
    }
    if (g_service_ready &&
        labtwin_environment_get_recovery_notice(&environment_event) == 0) {
        if (g_flow.alert_kind != LAB_UI_ALERT_RECOVERY_NOTICE)
            lab_ui_flow_show_alert(&g_flow, LAB_UI_ALERT_RECOVERY_NOTICE);
        fill_alert_snapshot(&snapshot, LAB_UI_ALERT_RECOVERY_NOTICE,
                            &environment_event);
        snapshot.page = g_flow.page;
        snapshot.experiment_picker_open = false;
        lab_ui_set_snapshot(g_ui, &snapshot);
        return;
    }
    if (g_flow.alert_kind == LAB_UI_ALERT_ENVIRONMENT ||
        g_flow.alert_kind == LAB_UI_ALERT_RECOVERY_NOTICE) {
        g_environment_event_id[0] = '\0';
        lab_ui_flow_finish_alert(&g_flow, have_view);
    }

    {
        lab_ui_timer_item_t expired_timer;
        if (g_service_ready && find_active_expired_timer(&expired_timer) == 0) {
            if (strcmp(g_timer_expiry_experiment_id,
                       expired_timer.experiment) != 0) {
                snprintf(g_timer_expiry_experiment_id,
                         sizeof(g_timer_expiry_experiment_id), "%s",
                         expired_timer.experiment);
                lab_ui_flow_show_alert(&g_flow, LAB_UI_ALERT_TIMER_EXPIRED);
            }
            if (g_flow.alert_kind == LAB_UI_ALERT_TIMER_EXPIRED) {
                fill_alert_snapshot(&snapshot, LAB_UI_ALERT_TIMER_EXPIRED,
                                    NULL);
                snapshot.page = g_flow.page;
                snapshot.experiment_picker_open = false;
                lab_ui_set_snapshot(g_ui, &snapshot);
                return;
            }
        } else {
            g_timer_expiry_experiment_id[0] = '\0';
        }
    }

#ifdef CONFIG_AI_AGENT_WAKEWORD
    {
        wake_session_snapshot_t wake;
        voice_action_snapshot_t action;
        wakeword_session_get_snapshot(&wake);
        voice_action_get_snapshot(&action);
        snapshot.microphone_ready = wake.state == WAKE_SESSION_PTT_READY ||
                                    wake.state == WAKE_SESSION_LOCAL_LISTENING;
        snapshot.voice_confirmation_pending =
            action.state == VOICE_ACTION_PENDING;
        if (g_voice_start_error != 0 &&
            g_flow.page == LAB_UI_PAGE_VOICE) {
            if (g_voice_start_error == -EBUSY) {
                snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                         "语音正在使用");
                snprintf(snapshot.voice_meta, sizeof(snapshot.voice_meta),
                         "请稍后再试");
                snprintf(snapshot.transcript, sizeof(snapshot.transcript),
                         "当前录音通道正在被其他语音任务使用。\n"
                         "请返回后稍候再试。");
            } else {
                snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                         "语音服务未配置");
                snprintf(snapshot.voice_meta, sizeof(snapshot.voice_meta),
                         "请先完成语音识别配置");
                snprintf(snapshot.transcript, sizeof(snapshot.transcript),
                         "当前设备尚未配置语音识别服务。\n"
                         "请配置 ASR 后再使用语音新建实验。");
            }
        } else {
        switch (wake.state) {
        case WAKE_SESSION_PTT_READY:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "点击麦克风开始问答"); break;
        case WAKE_SESSION_LOCAL_LISTENING:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "说“你好 vela”开始对话"); break;
        case WAKE_SESSION_WAKE_DETECTED:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "我在，请说…"); break;
        case WAKE_SESSION_COOLDOWN:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "本轮对话结束"); break;
        case WAKE_SESSION_COMMAND_RECORDING:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     g_new_experiment_voice ? "请说实验名称和步骤" :
                                              "正在聆听…"); break;
        case WAKE_SESSION_ASR:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "正在识别…"); break;
        case WAKE_SESSION_LLM:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "正在理解…"); break;
        case WAKE_SESSION_WAIT_CONFIRM:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "请确认操作"); break;
        case WAKE_SESSION_TTS:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "正在回答…"); break;
        default:
            snprintf(snapshot.voice_status, sizeof(snapshot.voice_status),
                     "语音未就绪"); break;
        }
        }
        if (g_voice_start_error == 0 &&
            action.state == VOICE_ACTION_PENDING) {
            snprintf(snapshot.voice_meta, sizeof(snapshot.voice_meta), "待执行");
            snprintf(snapshot.transcript, sizeof(snapshot.transcript),
                     "你说：%s\n将执行：%s", action.transcript, action.summary);
        } else if (g_voice_start_error == 0) {
            snprintf(snapshot.transcript, sizeof(snapshot.transcript),
                     "你：%s\n\n助手：%s", wake.transcript,
                     wake.reply[0] ? wake.reply : "等待回答…");
            if (wake.error[0])
                snprintf(snapshot.voice_meta, sizeof(snapshot.voice_meta),
                         "语音异常，文字已保留");
        }
        if (wake.state == WAKE_SESSION_PTT_READY ||
            wake.state == WAKE_SESSION_LOCAL_LISTENING) {
            /* Keep the conversation visible until the user returns. */
            g_new_experiment_voice = false;
            if (g_voice_start_error == 0) g_voice_dismissed = false;
        }
        if (!g_voice_dismissed &&
            wake.state != WAKE_SESSION_DISABLED &&
            wake.state != WAKE_SESSION_PTT_READY &&
            wake.state != WAKE_SESSION_LOCAL_LISTENING &&
            g_flow.page != LAB_UI_PAGE_VOICE) {
            g_flow.voice_return_page = g_flow.page;
            g_flow.voice_return_picker = g_flow.picker_open;
            g_flow.picker_open = false;
            g_flow.page = LAB_UI_PAGE_VOICE;
        }
    }
#endif

    if (have_view) fill_view_snapshot(&snapshot, &view);
    if (have_view && view.recovery_reason[0] &&
        g_flow.alert_kind == LAB_UI_ALERT_NONE)
        lab_ui_flow_show_alert(&g_flow, LAB_UI_ALERT_TIME_RECOVERY);
    if (g_flow.page == LAB_UI_PAGE_ALERT)
        fill_alert_snapshot(&snapshot, g_flow.alert_kind, NULL);
    snapshot.page = g_flow.page;
    snapshot.experiment_picker_open = g_flow.picker_open;
    lab_ui_set_snapshot(g_ui, &snapshot);
}

static bool focused_context(labtwin_experiment_view_t *view,
                            lab_ui_flow_context_t *context)
{
    bool have_view = g_service_ready && get_active_view(view) == 0;
    memset(context, 0, sizeof(*context));
    context->have_view = have_view;
    if (have_view) {
        context->paused = view->state == LABTWIN_STATE_PAUSED;
        context->last_step = view->step_count &&
                             view->step_index + 1 >= view->step_count;
    }
#ifdef CONFIG_AI_AGENT_WAKEWORD
    {
        voice_action_snapshot_t voice;
        voice_action_get_snapshot(&voice);
        context->voice_confirmation_pending =
            voice.state == VOICE_ACTION_PENDING;
    }
#endif
    return have_view;
}

void lab_ui_controller_handle_command(const lab_ui_action_t *action, void *arg)
{
    labtwin_experiment_view_t view;
    lab_ui_flow_context_t context;
    lab_ui_effect_t effect;
    bool have_view;
    (void)arg;
    if (!action) return;
    have_view = focused_context(&view, &context);
    effect = lab_ui_flow_handle(&g_flow, action, &context);
    switch (effect) {
    case LAB_UI_EFFECT_OPEN_SETTINGS:
        /* Opening board settings is the physical-presence confirmation for
         * first-time Portal pairing.  Paired devices return zero here. */
        admin_auth_open_pairing_window();
        if (g_settings_cb) g_settings_cb(g_settings_arg);
        return;
    case LAB_UI_EFFECT_START_VOICE:
    case LAB_UI_EFFECT_START_NEW_EXPERIMENT_VOICE:
        g_new_experiment_voice =
            effect == LAB_UI_EFFECT_START_NEW_EXPERIMENT_VOICE;
#ifdef CONFIG_AI_AGENT_WAKEWORD
        {
        int voice_start_ret;
        g_voice_dismissed = false;
        g_voice_start_error = 0;
        voice_start_ret = g_new_experiment_voice ?
            wakeword_session_start_new_experiment_ptt() :
            wakeword_session_start_ptt();
        if (voice_start_ret != 0) {
            g_new_experiment_voice = false;
            g_voice_start_error = voice_start_ret;
        }
        }
#else
        lab_ui_flow_finish_voice(&g_flow, have_view);
#endif
        break;
    case LAB_UI_EFFECT_FOCUS_EXPERIMENT:
        if (g_service_ready &&
            labtwin_focus_experiment(action->experiment_id) == 0 &&
            labtwin_get_focused_view(&view) == 0) {
            if (view.state == LABTWIN_STATE_READY)
                labtwin_ui_transition("start");
            g_flow.picker_open = false;
            g_flow.page = LAB_UI_PAGE_RUNNING;
        }
        break;
    case LAB_UI_EFFECT_TOGGLE_PAUSE:
        if (have_view)
            labtwin_ui_transition(view.state == LABTWIN_STATE_PAUSED ?
                                  "resume" : "pause");
        break;
    case LAB_UI_EFFECT_COMPLETE_STEP:
        if (have_view) labtwin_ui_transition("complete_step");
        break;
    case LAB_UI_EFFECT_FOCUS_NEXT:
        if (g_service_ready) labtwin_focus_next();
        break;
    case LAB_UI_EFFECT_ACK_ENVIRONMENT:
        if (g_environment_event_id[0]) {
            char input[96];
            char output[1024];
            snprintf(input, sizeof(input), "{\"event_id\":\"%s\"}",
                     g_environment_event_id);
            labtwin_environment_ack_json(input, output, sizeof(output),
                                         "touch");
        }
        break;
    case LAB_UI_EFFECT_RESUME_RECOVERY:
        if (have_view) labtwin_ui_transition("resume");
        lab_ui_flow_finish_alert(&g_flow, have_view);
        break;
    case LAB_UI_EFFECT_COMPLETE_EXPERIMENT:
        if (have_view) labtwin_ui_transition("complete");
        labtwin_focus_next();
        have_view = get_active_view(&view) == 0;
        lab_ui_flow_finish_alert(&g_flow, have_view);
        break;
    case LAB_UI_EFFECT_CANCEL_EXPERIMENT:
        if (have_view) labtwin_ui_transition("cancel");
        labtwin_focus_next();
        have_view = get_active_view(&view) == 0;
        lab_ui_flow_finish_alert(&g_flow, have_view);
        break;
    case LAB_UI_EFFECT_CANCEL_VOICE:
#ifdef CONFIG_AI_AGENT_WAKEWORD
        {
            voice_action_snapshot_t voice;
            voice_action_get_snapshot(&voice);
            if (voice.state == VOICE_ACTION_PENDING) voice_action_cancel();
            wakeword_session_cancel();
            g_voice_dismissed = true;
            g_voice_start_error = 0;
        }
#endif
        lab_ui_flow_finish_voice(&g_flow, have_view);
        g_new_experiment_voice = false;
        break;
    case LAB_UI_EFFECT_CONFIRM_VOICE:
#ifdef CONFIG_AI_AGENT_WAKEWORD
        {
            char result[1024];
            voice_action_confirm(result, sizeof(result));
            wakeword_session_on_confirmed();
            g_voice_dismissed = false;
            g_voice_start_error = 0;
        }
#endif
        lab_ui_flow_finish_voice(&g_flow, have_view);
        g_new_experiment_voice = false;
        break;
    default:
        break;
    }
    refresh(NULL);
}

int lab_ui_controller_start(lab_ui_t *ui,
                            lab_ui_controller_settings_cb_t settings_cb,
                            void *arg)
{
    int service_ret;
    if (!ui || g_ui) return -1;
    service_ret = labtwin_service_init();
    g_service_ready = service_ret == 0;
    g_service_retry_ticks = 0;
    g_ui = ui;
    g_settings_cb = settings_cb;
    g_settings_arg = arg;
    memset(&g_weather, 0, sizeof(g_weather));
    g_weather_valid = false;
    g_environment_event_id[0] = '\0';
    g_last_alert_tone_id[0] = '\0';
    g_last_alert_tone_ms = 0;
    g_alert_tone_count = 0;
    g_new_experiment_voice = false;
    g_timer_expiry_experiment_id[0] = '\0';
#ifdef CONFIG_AI_AGENT_WAKEWORD
    g_voice_dismissed = false;
    g_voice_start_error = 0;
#endif
    lab_ui_flow_init(&g_flow);
    if (service_ret != 0)
        LV_LOG_WARN("LabTwin service init failed (%d); degraded UI mode",
                    service_ret);
    if (weather_service_start() != 0)
        LV_LOG_WARN("Weather service failed to start");
    if (alert_audio_start() != 0)
        LV_LOG_WARN("Alert audio worker failed to start");
    g_refresh_timer = lv_timer_create(refresh, 1000, NULL);
    if (!g_refresh_timer) {
        alert_audio_stop();
        weather_service_stop();
        g_ui = NULL;
        g_settings_cb = NULL;
        g_settings_arg = NULL;
        return -1;
    }
    refresh(NULL);
    return 0;
}

void lab_ui_controller_stop(void)
{
    if (g_refresh_timer) lv_timer_delete(g_refresh_timer);
    g_refresh_timer = NULL;
    alert_audio_stop();
    weather_service_stop();
    g_ui = NULL;
    g_settings_cb = NULL;
    g_settings_arg = NULL;
    g_weather_valid = false;
    g_service_ready = false;
    g_service_retry_ticks = 0;
#ifdef CONFIG_AI_AGENT_WAKEWORD
    g_voice_start_error = 0;
#endif
    lab_ui_flow_init(&g_flow);
}
