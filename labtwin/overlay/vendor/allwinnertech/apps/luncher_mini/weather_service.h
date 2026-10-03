#ifndef LABTWIN_WEATHER_SERVICE_H
#define LABTWIN_WEATHER_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  char text[16];
  char temperature[16];
  char humidity[16];
  double latitude;
  double longitude;
  int weather_code;
  bool from_ip_location;
} weather_service_result_t;

int weather_service_start(void);
void weather_service_stop(void);
bool weather_service_poll(weather_service_result_t *result);
bool weather_service_clock_synced(void);
bool weather_service_parse_ip_json(const char *json, double *latitude,
                                   double *longitude);
bool weather_service_resolve_location_json(const char *json,
                                           double *latitude,
                                           double *longitude);
bool weather_service_parse_open_meteo_json(const char *json,
                                           double *temperature,
                                           double *humidity,
                                           int *weather_code);

#endif /* LABTWIN_WEATHER_SERVICE_H */
