#ifndef LOCALIZATION_H
#define LOCALIZATION_H

#include "telemetry.h"

/* Initialize emergency responder directory */
void localization_init(void);

/* Calculate great-circle distance between two GPS coordinates using Haversine formula (in km) */
double calculate_haversine_distance(double lat1, double lon1, double lat2, double lon2);

/* Calculate initial bearing (heading) from point 1 to point 2 in degrees (0-360) */
double calculate_bearing(double lat1, double lon1, double lat2, double lon2);

/* Find the nearest emergency trauma center/hospital for given coordinates */
bool localization_find_nearest(double lat, double lon, EmergencyStation *out_station);

/* Retrieve all registered emergency facilities for map visualization */
int localization_get_all_stations(EmergencyStation *out_stations, int max_stations);

/* Generate JSON array of emergency facilities */
int localization_stations_to_json(char *buffer, size_t max_size);

#endif /* LOCALIZATION_H */
