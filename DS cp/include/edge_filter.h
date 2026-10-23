#ifndef EDGE_FILTER_H
#define EDGE_FILTER_H

#include "telemetry.h"

typedef struct {
    /* TinyML Classifier Weights */
    float w_g_force;
    float w_jerk;
    float w_roll;
    float w_decel;
    float w_temp;
    float bias;

    /* Critical Decision Thresholds */
    float collision_g_threshold;     /* e.g., 4.5 G */
    float rollover_angle_threshold;  /* e.g., 55.0 degrees */
    float harsh_brake_g_threshold;   /* e.g., -0.85 G */
    float overheat_temp_c;           /* e.g., 115.0 °C */

    /* Historical state for derivative/jerk calculation */
    float prev_accel_x;
    float prev_accel_y;
    float prev_accel_z;
    float prev_speed_kmh;
    uint64_t prev_timestamp_ms;
} EdgeFilterModel;

/* Initialize edge filter model parameters */
void edge_filter_init(EdgeFilterModel *model);

/* Process a single incoming telemetry packet. Computes total_g, anomaly_score,
   anomaly_type, severity, and returns inference execution time in microseconds. */
float edge_filter_process(EdgeFilterModel *model, TelemetryPacket *packet);

/* Check whether a packet indicates an emergency situation requiring buffer freeze */
bool edge_filter_is_emergency(const TelemetryPacket *packet);

#endif /* EDGE_FILTER_H */
