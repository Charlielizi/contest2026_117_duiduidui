#include "weather_service.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
  static const char response[] =
    "{\"current_units\":{\"temperature_2m\":\"C\","
    "\"relative_humidity_2m\":\"%\",\"weather_code\":\"wmo code\"},"
    "\"current\":{\"temperature_2m\":26.4,"
    "\"relative_humidity_2m\":77,\"weather_code\":3}}";
  double latitude;
  double longitude;
  double temperature;
  double humidity;
  int code;

  assert(weather_service_parse_ip_json(
           "{\"latitude\":39.9,\"longitude\":116.4}",
           &latitude, &longitude));
  assert(fabs(latitude - 39.9) < 0.001);
  assert(fabs(longitude - 116.4) < 0.001);
  assert(!weather_service_parse_ip_json(
           "{\"latitude\":139.9,\"longitude\":116.4}",
           &latitude, &longitude));
  assert(!weather_service_resolve_location_json("{invalid",
                                                 &latitude, &longitude));
  assert(fabs(latitude - 23.0207) < 0.001);
  assert(fabs(longitude - 113.7518) < 0.001);
  assert(weather_service_parse_open_meteo_json(
           response, &temperature, &humidity, &code));
  assert(fabs(temperature - 26.4) < 0.001);
  assert(fabs(humidity - 77.0) < 0.001);
  assert(code == 3);
  assert(!weather_service_parse_open_meteo_json(
           "{\"current_units\":{\"temperature_2m\":\"C\"}}",
           &temperature, &humidity, &code));
  assert(!weather_service_parse_open_meteo_json(
           "{\"current\":{\"temperature_2m\":26.4,"
           "\"relative_humidity_2m\":177,\"weather_code\":3}}",
           &temperature, &humidity, &code));
  /* A failed response must not poison the next valid response after the
   * network recovers. */
  assert(!weather_service_parse_open_meteo_json("{invalid",
                                                 &temperature, &humidity,
                                                 &code));
  assert(weather_service_parse_open_meteo_json(
           response, &temperature, &humidity, &code));
  puts("weather_service_host_test: PASS");
  return 0;
}
