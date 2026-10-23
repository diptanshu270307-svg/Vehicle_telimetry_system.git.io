#include "edge_filter.h"
#include <math.h>
#include <time.h>
#include <stdlib.h>

static inline float sigmoid(float z) {
    if (z > 10.0f) return 1.0f;
    if (z < -10.0f) return 0.0f;
    return 1.0f / (1.0f + expf(-z));
}

void edge_filter_init(EdgeFilterModel *model) {
    if (!model) return;
    /* Pre-calibrated edge classifier weights */
    model->w_g_force = 2.4f;
    model->w_jerk = 1.8f;
    model->w_roll = 1.6f;
    model->w_decel = 1.3f;
    model->w_temp = 0.9f;
    model->bias = -3.2f;

    model->collision_g_threshold = 4.0f;     /* 4.0G+ triggers collision alert */
    model->rollover_angle_threshold = 50.0f; /* >50° tilt triggers rollover alert */
    model->harsh_brake_g_threshold = -0.75f; /* Harsh deceleration */
    model->overheat_temp_c = 115.0f;

    model->prev_accel_x = 0.0f;
    model->prev_accel_y = 0.0f;
    model->prev_accel_z = 1.0f;
    model->prev_speed_kmh = 0.0f;
    model->prev_timestamp_ms = 0;
}

float edge_filter_process(EdgeFilterModel *model, TelemetryPacket *packet) {
    if (!model || !packet) return 0.0f;

    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* 1. Vector magnitude of acceleration (in G's) */
    packet->total_g = sqrtf(packet->accel_x * packet->accel_x +
                            packet->accel_y * packet->accel_y +
                            packet->accel_z * packet->accel_z);

    /* 2. Kinematic derivative (jerk and decel) */
    float dt = 0.05f; /* default 50ms (20Hz) */
    if (model->prev_timestamp_ms > 0 && packet->timestamp_ms > model->prev_timestamp_ms) {
        dt = (float)(packet->timestamp_ms - model->prev_timestamp_ms) / 1000.0f;
        if (dt < 0.005f) dt = 0.005f;
    }

    float prev_g = sqrtf(model->prev_accel_x * model->prev_accel_x +
                         model->prev_accel_y * model->prev_accel_y +
                         model->prev_accel_z * model->prev_accel_z);
    float jerk = fabsf(packet->total_g - prev_g) / dt;

    float decel = 0.0f;
    if (packet->speed_kmh < model->prev_speed_kmh) {
        /* Deceleration in G's: (dv/dt) / 9.81 */
        float dv_ms = (model->prev_speed_kmh - packet->speed_kmh) / 3.6f;
        decel = dv_ms / (dt * 9.81f);
    }

    /* 3. Normalized feature vector */
    float feat_g = packet->total_g / 5.0f;
    float feat_jerk = jerk / 30.0f;
    float feat_roll = fabsf(packet->gyro_roll) / 90.0f;
    float feat_decel = decel / 1.5f;
    float feat_temp = (packet->engine_temp_c > 90.0f) ? (packet->engine_temp_c - 90.0f) / 30.0f : 0.0f;

    /* 4. TinyML inference: calibrated logistic decision boundary */
    float z = (model->w_g_force * feat_g) +
              (model->w_jerk * feat_jerk) +
              (model->w_roll * feat_roll) +
              (model->w_decel * feat_decel) +
              (model->w_temp * feat_temp) +
              model->bias;
    packet->anomaly_score = sigmoid(z);

    /* 5. Deterministic classification & safety trigger */
    if (packet->total_g >= model->collision_g_threshold) {
        packet->anomaly_type = ANOMALY_CRASH_COLLISION;
        packet->severity = (packet->total_g >= 6.5f) ? SEVERITY_CRITICAL : SEVERITY_HIGH;
        if (packet->anomaly_score < 0.95f) packet->anomaly_score = 0.95f;
    } else if (fabsf(packet->gyro_roll) >= model->rollover_angle_threshold) {
        packet->anomaly_type = ANOMALY_ROLLOVER;
        packet->severity = SEVERITY_CRITICAL;
        if (packet->anomaly_score < 0.92f) packet->anomaly_score = 0.92f;
    } else if (packet->speed_kmh > 45.0f && packet->total_g > 2.8f) {
        packet->anomaly_type = ANOMALY_HIGH_SPEED_IMPACT;
        packet->severity = SEVERITY_CRITICAL;
        if (packet->anomaly_score < 0.88f) packet->anomaly_score = 0.88f;
    } else if (packet->accel_x <= model->harsh_brake_g_threshold || (packet->brake_pressure > 85.0f && decel > 0.8f)) {
        packet->anomaly_type = ANOMALY_HARSH_BRAKE;
        packet->severity = (decel > 1.2f) ? SEVERITY_HIGH : SEVERITY_MEDIUM;
        if (packet->anomaly_score < 0.65f) packet->anomaly_score = 0.65f;
    } else if (packet->engine_temp_c >= model->overheat_temp_c) {
        packet->anomaly_type = ANOMALY_THERMAL_RUNAWAY;
        packet->severity = (packet->engine_temp_c >= 125.0f) ? SEVERITY_CRITICAL : SEVERITY_HIGH;
        if (packet->anomaly_score < 0.75f) packet->anomaly_score = 0.75f;
    } else {
        packet->anomaly_type = ANOMALY_NONE;
        packet->severity = SEVERITY_NORMAL;
    }

    /* Update model state for subsequent derivative calculation */
    model->prev_accel_x = packet->accel_x;
    model->prev_accel_y = packet->accel_y;
    model->prev_accel_z = packet->accel_z;
    model->prev_speed_kmh = packet->speed_kmh;
    model->prev_timestamp_ms = packet->timestamp_ms;

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    float latency_us = (float)(end_ts.tv_sec - start_ts.tv_sec) * 1000000.0f +
                       (float)(end_ts.tv_nsec - start_ts.tv_nsec) / 1000.0f;
    return latency_us;
}

bool edge_filter_is_emergency(const TelemetryPacket *packet) {
    if (!packet) return false;
    return (packet->severity >= SEVERITY_HIGH &&
            (packet->anomaly_type == ANOMALY_CRASH_COLLISION ||
             packet->anomaly_type == ANOMALY_ROLLOVER ||
             packet->anomaly_type == ANOMALY_HIGH_SPEED_IMPACT));
}
