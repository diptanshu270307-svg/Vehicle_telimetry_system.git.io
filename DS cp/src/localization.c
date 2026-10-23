#include "localization.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define EARTH_RADIUS_KM 6371.0
#define PI 3.14159265358979323846

static EmergencyStation g_stations[MAX_STATIONS];
static int g_station_count = 0;

static inline double deg2rad(double deg) {
    return deg * (PI / 180.0);
}

static inline double rad2deg(double rad) {
    return rad * (180.0 / PI);
}

void localization_init(void) {
    g_station_count = 0;

    /* Realistic emergency responders in urban driving corridor (Pune / Metro hub) */
    strncpy(g_stations[0].name, "Ruby Hall Clinic Trauma Hub", sizeof(g_stations[0].name));
    strncpy(g_stations[0].type, "Level-1 Trauma & Neuro Center", sizeof(g_stations[0].type));
    g_stations[0].latitude = 18.5328;
    g_stations[0].longitude = 73.8772;
    strncpy(g_stations[0].phone, "+91-20-66455100", sizeof(g_stations[0].phone));

    strncpy(g_stations[1].name, "Sancheti Orthopedic & Trauma Hospital", sizeof(g_stations[1].name));
    strncpy(g_stations[1].type, "Specialized Spine & Trauma", sizeof(g_stations[1].type));
    g_stations[1].latitude = 18.5284;
    g_stations[1].longitude = 73.8523;
    strncpy(g_stations[1].phone, "+91-20-28999888", sizeof(g_stations[1].phone));

    strncpy(g_stations[2].name, "Jehangir Hospital Emergency Dept", sizeof(g_stations[2].name));
    strncpy(g_stations[2].type, "Tertiary Care & Cardiac Center", sizeof(g_stations[2].type));
    g_stations[2].latitude = 18.5305;
    g_stations[2].longitude = 73.8745;
    strncpy(g_stations[2].phone, "+91-20-66811000", sizeof(g_stations[2].phone));

    strncpy(g_stations[3].name, "Poona Hospital Emergency Unit", sizeof(g_stations[3].name));
    strncpy(g_stations[3].type, "Critical Emergency Care", sizeof(g_stations[3].type));
    g_stations[3].latitude = 18.5146;
    g_stations[3].longitude = 73.8447;
    strncpy(g_stations[3].phone, "+91-20-24331707", sizeof(g_stations[3].phone));

    strncpy(g_stations[4].name, "Deenanath Mangeshkar Trauma Center", sizeof(g_stations[4].name));
    strncpy(g_stations[4].type, "Super-Specialty Acute Center", sizeof(g_stations[4].type));
    g_stations[4].latitude = 18.5029;
    g_stations[4].longitude = 73.8315;
    strncpy(g_stations[4].phone, "+91-20-40151000", sizeof(g_stations[4].phone));

    strncpy(g_stations[5].name, "Sahyadri Super Speciality ER", sizeof(g_stations[5].name));
    strncpy(g_stations[5].type, "Advanced Resuscitation Unit", sizeof(g_stations[5].type));
    g_stations[5].latitude = 18.5117;
    g_stations[5].longitude = 73.8344;
    strncpy(g_stations[5].phone, "+91-20-67215000", sizeof(g_stations[5].phone));

    g_station_count = 6;
}

double calculate_haversine_distance(double lat1, double lon1, double lat2, double lon2) {
    double dLat = deg2rad(lat2 - lat1);
    double dLon = deg2rad(lon2 - lon1);
    double rLat1 = deg2rad(lat1);
    double rLat2 = deg2rad(lat2);

    double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
               sin(dLon / 2.0) * sin(dLon / 2.0) * cos(rLat1) * cos(rLat2);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return EARTH_RADIUS_KM * c;
}

double calculate_bearing(double lat1, double lon1, double lat2, double lon2) {
    double dLon = deg2rad(lon2 - lon1);
    double rLat1 = deg2rad(lat1);
    double rLat2 = deg2rad(lat2);

    double y = sin(dLon) * cos(rLat2);
    double x = cos(rLat1) * sin(rLat2) - sin(rLat1) * cos(rLat2) * cos(dLon);
    double b = rad2deg(atan2(y, x));
    if (b < 0.0) b += 360.0;
    return b;
}

bool localization_find_nearest(double lat, double lon, EmergencyStation *out_station) {
    if (!out_station || g_station_count == 0) return false;

    double min_dist = 1e9;
    int best_idx = -1;

    for (int i = 0; i < g_station_count; i++) {
        double dist = calculate_haversine_distance(lat, lon, g_stations[i].latitude, g_stations[i].longitude);
        if (dist < min_dist) {
            min_dist = dist;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        *out_station = g_stations[best_idx];
        out_station->distance_km = min_dist;
        return true;
    }
    return false;
}

int localization_get_all_stations(EmergencyStation *out_stations, int max_stations) {
    if (!out_stations || max_stations <= 0) return 0;
    int n = (g_station_count < max_stations) ? g_station_count : max_stations;
    for (int i = 0; i < n; i++) {
        out_stations[i] = g_stations[i];
    }
    return n;
}

int localization_stations_to_json(char *buffer, size_t max_size) {
    if (!buffer || max_size < 10) return 0;

    size_t offset = 0;
    offset += snprintf(buffer + offset, max_size - offset, "[");

    for (int i = 0; i < g_station_count && offset < max_size - 100; i++) {
        if (i > 0) offset += snprintf(buffer + offset, max_size - offset, ",");
        offset += snprintf(buffer + offset, max_size - offset,
            "{\"name\":\"%s\",\"type\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"phone\":\"%s\"}",
            g_stations[i].name,
            g_stations[i].type,
            g_stations[i].latitude,
            g_stations[i].longitude,
            g_stations[i].phone);
    }

    offset += snprintf(buffer + offset, max_size - offset, "]");
    return (int)offset;
}
