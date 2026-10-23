#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TELEMETRY_WINDOW_SIZE 100    /* Ring buffer capacity: 100 packets = 5 s at 20 Hz        */
#define POST_TRIGGER_PACKETS  20     /* Keep recording 20 packets (1 s) AFTER impact, then freeze */
#define SAMPLE_PERIOD_MS      50     /* 20 Hz sensor sampling                                    */
#define MAX_STATIONS 8
#define DEFAULT_SERVER_PORT 8080
#define DEFAULT_BLACKBOX_PATH "./data/blackbox.bin"

typedef enum {
    ANOMALY_NONE = 0,
    ANOMALY_CRASH_COLLISION = 1,
    ANOMALY_ROLLOVER = 2,
    ANOMALY_HARSH_BRAKE = 3,
    ANOMALY_THERMAL_RUNAWAY = 4,
    ANOMALY_HIGH_SPEED_IMPACT = 5
} AnomalyType;

typedef enum {
    SEVERITY_NORMAL = 0,
    SEVERITY_LOW = 1,
    SEVERITY_MEDIUM = 2,
    SEVERITY_HIGH = 3,
    SEVERITY_CRITICAL = 4
} SeverityLevel;

/* ---------------------------------------------------------------------------
 * DATA STRUCTURE #1: struct TelemetryPacket
 * One sensor snapshot captured at the edge every 50 ms.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t timestamp_ms;
    float speed_kmh;
    float rpm;
    float accel_x;       /* Longitudinal acceleration in G (negative = braking/impact) */
    float accel_y;       /* Lateral acceleration in G */
    float accel_z;       /* Vertical acceleration in G (1.0 = gravity at rest) */
    float total_g;       /* Vector magnitude: sqrt(ax^2 + ay^2 + az^2) */
    float gyro_pitch;    /* Pitch angle in degrees */
    float gyro_roll;     /* Roll angle in degrees */
    float gyro_yaw;      /* Heading 0-360 degrees */
    float brake_pressure;/* 0-100 % */
    float engine_temp_c; /* Coolant temperature in Celsius */
    double latitude;     /* GPS latitude */
    double longitude;    /* GPS longitude */
    float anomaly_score; /* Classifier output 0.0 - 1.0 */
    AnomalyType anomaly_type;
    SeverityLevel severity;
} TelemetryPacket;

/* Emergency responder facility used by the localization module */
typedef struct {
    char name[64];
    char type[32];
    double latitude;
    double longitude;
    char phone[24];
    double distance_km;  /* Filled in at query time */
} EmergencyStation;

/* Payload stored inside each linked-list node (the "black box" of one incident) */
typedef struct {
    int incident_id;
    uint64_t timestamp_ms;
    AnomalyType type;
    SeverityLevel severity;
    float peak_g;
    float impact_speed_kmh;
    float roll_angle;
    double latitude;
    double longitude;
    EmergencyStation nearest_station;
    char description[128];
    int packet_count;
    int trigger_index;   /* Index inside packets[] where the anomaly was first detected */
    TelemetryPacket packets[TELEMETRY_WINDOW_SIZE]; /* Pre-impact + post-impact window */
} IncidentRecord;

/* Live state shared between the simulation thread and the HTTP thread */
typedef struct {
    char vehicle_id[32];
    volatile bool is_running;
    volatile bool is_frozen;
    volatile int current_scenario;   /* 0 Normal, 1 Collision, 2 Rollover, 3 Harsh Brake, 4 Overheat */
    volatile int scenario_epoch;     /* Incremented every time a scenario is (re)triggered */
    volatile bool reset_requested;
    volatile int post_capture_remaining; /* >0 while recording post-impact packets */
    volatile int latest_incident_id;
    uint64_t total_packets_processed;
    uint64_t total_anomalies_detected;
    float current_inference_time_us;
} VehicleState;

const char* anomaly_type_to_string(AnomalyType type);
const char* severity_to_string(SeverityLevel severity);

#endif /* TELEMETRY_H */
