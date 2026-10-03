#include "weather_service.h"

#include "infra/vela_tls.h"
#include "cJSON.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define WEATHER_IP_HOST       "ipapi.co"
#define WEATHER_IP_PATH       "/json/"
#define WEATHER_METEO_HOST   "api.open-meteo.com"
#define WEATHER_FALLBACK_LATITUDE  23.0207
#define WEATHER_FALLBACK_LONGITUDE 113.7518
#define WEATHER_BODY_SIZE     4096
#define WEATHER_REFRESH_SEC   (15 * 60)
#define WEATHER_RETRY_SEC     60
#define WEATHER_START_DELAY_SEC 10
#define WEATHER_THREAD_STACK  (32 * 1024)
#define CLOCK_SYNC_INTERVAL_SEC (6 * 60 * 60)

static const char *g_months[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

typedef struct
{
  pthread_t thread;
  pthread_mutex_t lock;
  bool running;
  bool stop;
  bool pending;
  bool clock_synced;
  weather_service_result_t result;
} weather_service_state_t;

static weather_service_state_t g_weather = {
  .lock = PTHREAD_MUTEX_INITIALIZER
};

static bool service_should_stop(void)
{
  bool stop;
  pthread_mutex_lock(&g_weather.lock);
  stop = g_weather.stop;
  pthread_mutex_unlock(&g_weather.lock);
  return stop;
}

static void service_sleep(int seconds)
{
  while (seconds-- > 0 && !service_should_stop())
    {
      sleep(1);
    }
}

static bool object_number(const cJSON *object, const char *key, double *value)
{
  const cJSON *item;
  if (!object || !key || !value)
    return false;
  item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble))
    return false;
  *value = item->valuedouble;
  return true;
}

bool weather_service_parse_ip_json(const char *json, double *latitude,
                                   double *longitude)
{
  cJSON *root;
  bool ok;
  if (!json || !latitude || !longitude) return false;
  root = cJSON_Parse(json);
  if (!root) return false;
  ok = object_number(root, "latitude", latitude) &&
       object_number(root, "longitude", longitude) &&
       *latitude >= -90.0 && *latitude <= 90.0 &&
       *longitude >= -180.0 && *longitude <= 180.0;
  cJSON_Delete(root);
  return ok;
}

bool weather_service_resolve_location_json(const char *json,
                                           double *latitude,
                                           double *longitude)
{
  if (weather_service_parse_ip_json(json, latitude, longitude)) return true;
  if (latitude) *latitude = WEATHER_FALLBACK_LATITUDE;
  if (longitude) *longitude = WEATHER_FALLBACK_LONGITUDE;
  return false;
}

bool weather_service_parse_open_meteo_json(const char *json,
                                           double *temperature,
                                           double *humidity,
                                           int *weather_code)
{
  cJSON *root;
  cJSON *current;
  double code;
  bool ok;
  if (!json || !temperature || !humidity || !weather_code) return false;
  root = cJSON_Parse(json);
  if (!root) return false;
  current = cJSON_GetObjectItemCaseSensitive(root, "current");
  ok = cJSON_IsObject(current) &&
       object_number(current, "temperature_2m", temperature) &&
       object_number(current, "relative_humidity_2m", humidity) &&
       object_number(current, "weather_code", &code) &&
       *temperature >= -100.0 && *temperature <= 100.0 &&
       *humidity >= 0.0 && *humidity <= 100.0 &&
       code >= 0.0 && code <= 99.0;
  if (ok) *weather_code = (int)code;
  cJSON_Delete(root);
  return ok;
}

static const char *weather_text(int code)
{
  if (code == 0) return "晴";
  if (code <= 3) return "多云";
  if (code == 45 || code == 48) return "雾";
  if (code <= 57) return "毛毛雨";
  if (code <= 67 || (code >= 80 && code <= 82)) return "下雨";
  if (code <= 77 || (code >= 85 && code <= 86)) return "下雪";
  if (code >= 95) return "雷雨";
  return "多云";
}

static bool sync_beijing_time(void)
{
  char date_value[64] = {0};
  char month_text[4] = {0};
  struct tm utc_time;
  struct timeval tv;
  time_t epoch;
  int day;
  int year;
  int hour;
  int minute;
  int second;
  int month = -1;
  int i;

  if (vela_https_head_date("www.baidu.com", "443", "/", date_value,
                           sizeof(date_value)) != 0 &&
      vela_https_head_date(WEATHER_METEO_HOST, "443", "/", date_value,
                           sizeof(date_value)) != 0)
    return false;
  if (sscanf(date_value, "%*[^,], %d %3s %d %d:%d:%d",
             &day, month_text, &year, &hour, &minute, &second) != 6)
    return false;
  for (i = 0; i < 12; i++)
    {
      if (strcmp(month_text, g_months[i]) == 0)
        {
          month = i;
          break;
        }
    }
  if (month < 0)
    return false;

  memset(&utc_time, 0, sizeof(utc_time));
  utc_time.tm_year = year - 1900;
  utc_time.tm_mon = month;
  utc_time.tm_mday = day;
  utc_time.tm_hour = hour;
  utc_time.tm_min = minute;
  utc_time.tm_sec = second;
  epoch = timegm(&utc_time);
  if (epoch < 0)
    return false;

  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  if (settimeofday(&tv, NULL) < 0)
    return false;
  pthread_mutex_lock(&g_weather.lock);
  g_weather.clock_synced = true;
  pthread_mutex_unlock(&g_weather.lock);
  syslog(LOG_INFO, "weather: Beijing clock synchronized from HTTPS Date\n");
  return true;
}

static bool fetch_weather(weather_service_result_t *result)
{
  char *response;
  char path[256];
  double latitude = WEATHER_FALLBACK_LATITUDE;  /* Dongguan fallback. */
  double longitude = WEATHER_FALLBACK_LONGITUDE;
  double temperature;
  double humidity;
  int weather_code;
  bool from_ip = false;
  int status;

  /* Two 4 KiB response arrays used to live on the worker stack.  NuttX's
   * default pthread stack is too small for those buffers plus mbedTLS and can
   * terminate the launcher before its first frame is presented. */
  response = malloc(WEATHER_BODY_SIZE);
  if (!response)
    return false;

  status = vela_https_get(WEATHER_IP_HOST, "443", WEATHER_IP_PATH,
                          response, WEATHER_BODY_SIZE);
  from_ip = weather_service_resolve_location_json(
              status >= 200 && status < 300 ? response : NULL,
              &latitude, &longitude);

  snprintf(path, sizeof(path),
           "/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code"
           "&timezone=auto",
           latitude, longitude);
  status = vela_https_get(WEATHER_METEO_HOST, "443", path,
                          response, WEATHER_BODY_SIZE);
  if (status < 200 || status >= 300 ||
      !weather_service_parse_open_meteo_json(response, &temperature,
                                             &humidity, &weather_code))
    {
      free(response);
      return false;
    }

  memset(result, 0, sizeof(*result));
  result->weather_code = weather_code;
  snprintf(result->text, sizeof(result->text), "%s",
           weather_text(result->weather_code));
  snprintf(result->temperature, sizeof(result->temperature), "%.1f℃",
           temperature);
  snprintf(result->humidity, sizeof(result->humidity), "%.0f%%", humidity);
  result->latitude = latitude;
  result->longitude = longitude;
  result->from_ip_location = from_ip;
  free(response);
  return true;
}

static void publish(const weather_service_result_t *result)
{
  pthread_mutex_lock(&g_weather.lock);
  g_weather.result = *result;
  g_weather.pending = true;
  pthread_mutex_unlock(&g_weather.lock);
}

static void *weather_worker(void *arg)
{
  time_t next_clock_sync = 0;
  time_t next_weather_fetch = 0;
  (void)arg;
  /* Let LCD, input, storage and network services finish booting first. */
  service_sleep(WEATHER_START_DELAY_SEC);
  while (!service_should_stop())
    {
      weather_service_result_t result;
      time_t now = time(NULL);
      if (next_clock_sync == 0 || now >= next_clock_sync)
        {
          if (sync_beijing_time())
            next_clock_sync = time(NULL) + CLOCK_SYNC_INTERVAL_SEC;
          else
            next_clock_sync = now + WEATHER_RETRY_SEC;
        }
      if (next_weather_fetch == 0 || now >= next_weather_fetch)
        {
          if (fetch_weather(&result))
            {
              publish(&result);
              syslog(LOG_INFO, "weather: %s %s at %.4f,%.4f%s\n",
                     result.text, result.temperature, result.latitude,
                     result.longitude,
                     result.from_ip_location ? " (ip)" : " (fallback)");
              next_weather_fetch = now + WEATHER_REFRESH_SEC;
            }
          else
            {
              syslog(LOG_WARNING,
                     "weather: Open-Meteo request failed; retrying\n");
              next_weather_fetch = now + WEATHER_RETRY_SEC;
            }
        }
      now = time(NULL);
      {
        time_t next = next_clock_sync < next_weather_fetch ?
                      next_clock_sync : next_weather_fetch;
        int delay = next > now ? (int)(next - now) : 1;
        service_sleep(delay);
      }
    }
  return NULL;
}

int weather_service_start(void)
{
  pthread_attr_t attr;
  int ret;
  pthread_mutex_lock(&g_weather.lock);
  if (g_weather.running)
    {
      pthread_mutex_unlock(&g_weather.lock);
      return 0;
    }
  g_weather.stop = false;
  g_weather.pending = false;
  g_weather.clock_synced = false;
  pthread_mutex_unlock(&g_weather.lock);

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      syslog(LOG_ERR, "weather: pthread_attr_init failed: %d\n", ret);
      return -1;
    }
  ret = pthread_attr_setstacksize(&attr, WEATHER_THREAD_STACK);
  if (ret == 0)
    ret = pthread_create(&g_weather.thread, &attr, weather_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      syslog(LOG_ERR, "weather: pthread_create failed: %d\n", ret);
      return -1;
    }
  pthread_mutex_lock(&g_weather.lock);
  g_weather.running = true;
  pthread_mutex_unlock(&g_weather.lock);
  return 0;
}

void weather_service_stop(void)
{
  pthread_t thread;
  bool running;

  pthread_mutex_lock(&g_weather.lock);
  running = g_weather.running;
  thread = g_weather.thread;
  g_weather.stop = true;
  pthread_mutex_unlock(&g_weather.lock);
  if (!running)
    return;
  pthread_join(thread, NULL);
  pthread_mutex_lock(&g_weather.lock);
  g_weather.running = false;
  pthread_mutex_unlock(&g_weather.lock);
}

bool weather_service_poll(weather_service_result_t *result)
{
  bool available = false;
  if (!result)
    return false;
  pthread_mutex_lock(&g_weather.lock);
  if (g_weather.pending)
    {
      *result = g_weather.result;
      g_weather.pending = false;
      available = true;
    }
  pthread_mutex_unlock(&g_weather.lock);
  return available;
}

bool weather_service_clock_synced(void)
{
  bool synced;
  pthread_mutex_lock(&g_weather.lock);
  synced = g_weather.clock_synced;
  pthread_mutex_unlock(&g_weather.lock);
  return synced;
}
