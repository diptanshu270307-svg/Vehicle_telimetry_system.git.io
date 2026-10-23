/*
 * Telemetry & Anomaly Reconstruction Engine - main orchestrator
 *
 *  Sensor thread (20 Hz)                     HTTP thread(s)
 *  ---------------------                     --------------
 *  generate TelemetryPacket  ---------->     SSE live stream to dashboard
 *  edge_filter_process()  (classify)         REST: status / incidents / reconstruct
 *  cq_enqueue()           (ring buffer)      POST: simulate / reset
 *  on emergency:
 *     - instant alert + localization
 *     - keep recording POST_TRIGGER_PACKETS
 *     - freeze ring buffer, copy it into a
 *       new linked-list node, fsync to disk
 */
#include "telemetry.h"
#include "circular_queue.h"
#include "linked_list.h"
#include "edge_filter.h"
#include "localization.h"
#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------- Global engine state ---------------- */
static CircularQueue   g_queue;       /* DS #2: ring buffer  */
static IncidentList    g_incidents;   /* DS #3: linked list  */
static EdgeFilterModel g_filter;
static VehicleState    g_state;
static HttpServerConfig g_server_config;
static const char     *g_blackbox_path = DEFAULT_BLACKBOX_PATH;

static volatile sig_atomic_t g_keep_running = 1;
static pthread_t g_sim_thread;

/* Pending (critical) incident waiting for its post-impact packets */
static IncidentRecord g_pending;
static bool g_has_pending = false;
static int  g_pending_trigger_offset = 0;   /* packets recorded since trigger */

/* Scratch record for non-critical events (harsh brake / overheating) */
static IncidentRecord g_minor;
static AnomalyType g_minor_active = ANOMALY_NONE;

static void handle_sigint(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + (uint64_t)(ts.tv_nsec / 1000000);
}

static float jitter(float amplitude) {
    return (((float)(rand() % 1000) / 1000.0f) * 2.0f - 1.0f) * amplitude;
}

/* Fill the metadata of an incident from the packet that triggered it */
static void build_incident(IncidentRecord *inc, const TelemetryPacket *pkt, float pre_impact_speed) {
    memset(inc, 0, sizeof(*inc));
    inc->timestamp_ms     = pkt->timestamp_ms;
    inc->type             = pkt->anomaly_type;
    inc->severity         = pkt->severity;
    inc->peak_g           = pkt->total_g;
    inc->impact_speed_kmh = pre_impact_speed;
    inc->roll_angle       = pkt->gyro_roll;
    inc->latitude         = pkt->latitude;
    inc->longitude        = pkt->longitude;

    /* Instant localization (Haversine) */
    EmergencyStation st;
    if (localization_find_nearest(pkt->latitude, pkt->longitude, &st)) {
        inc->nearest_station = st;
    }
}

static void write_description(IncidentRecord *inc) {
    switch (inc->type) {
        case ANOMALY_CRASH_COLLISION:
        case ANOMALY_HIGH_SPEED_IMPACT:
            snprintf(inc->description, sizeof(inc->description),
                     "Frontal collision: %.1fG peak at %.0f km/h", inc->peak_g, inc->impact_speed_kmh);
            break;
        case ANOMALY_ROLLOVER:
            snprintf(inc->description, sizeof(inc->description),
                     "Vehicle rollover: %.0f deg roll at %.0f km/h", fabsf(inc->roll_angle), inc->impact_speed_kmh);
            break;
        case ANOMALY_HARSH_BRAKE:
            snprintf(inc->description, sizeof(inc->description),
                     "Harsh braking event from %.0f km/h", inc->impact_speed_kmh);
            break;
        case ANOMALY_THERMAL_RUNAWAY:
            snprintf(inc->description, sizeof(inc->description),
                     "Engine overheating detected");
            break;
        default:
            snprintf(inc->description, sizeof(inc->description), "Anomaly");
    }
}

/* Copy ring buffer into the record, insert into the linked list, persist to disk */
static void commit_incident(IncidentRecord *inc, int packets_after_trigger) {
    inc->packet_count  = cq_get_all_ordered(&g_queue, inc->packets, TELEMETRY_WINDOW_SIZE);
    inc->trigger_index = inc->packet_count - 1 - packets_after_trigger;
    if (inc->trigger_index < 0) inc->trigger_index = 0;
    write_description(inc);

    int id = list_insert_incident(&g_incidents, *inc);
    inc->incident_id = id;
    g_state.latest_incident_id = id;
    g_state.total_anomalies_detected++;

    bool saved = list_append_to_file(inc, g_blackbox_path);

    printf("[LINKED LIST] Node #%d committed: %s | %d packets (impact at index %d) | disk: %s\n",
           id, inc->description, inc->packet_count, inc->trigger_index,
           saved ? "saved + fsync" : "WRITE FAILED");

    http_server_broadcast_incident(inc);
}

/* Gently accelerate up to target speed. Returns true while still accelerating. */
static bool approach_speed(float *speed, TelemetryPacket *pkt, float target) {
    if (*speed >= target) return false;
    *speed += 1.5f;                       /* 1.5 km/h per 50 ms ~= 0.85 G */
    pkt->accel_x = 0.85f + jitter(0.03f);
    pkt->accel_y = jitter(0.03f);
    pkt->accel_z = 1.0f + jitter(0.02f);
    return true;
}

/* ===================================================================== */
/* Sensor simulation + edge processing thread (20 Hz)                    */
/* ===================================================================== */
static void* simulation_worker(void *arg) {
    (void)arg;

    double lat = 18.5204, lon = 73.8567;     /* Pune */
    float speed = 0.0f, rpm = 800.0f;
    float roll = 0.0f, pitch = 1.0f, yaw = 45.0f, temp = 88.0f;
    float last_speed = 0.0f;

    int  scenario_tick = 0;
    bool approach_done = false;
    int  last_epoch = -1;

    /* Collision deceleration profile (G) over three 50 ms samples.
       Total delta-v = (14+11+6) * 9.81 * 0.05 s ~= 15.2 m/s ~= 55 km/h */
    const float crash_ax[3] = { -14.0f, -11.0f, -6.0f };
    const float crash_ay[3] = {   3.0f,   2.2f,  1.0f };
    const float crash_az[3] = {   2.0f,   1.5f,  1.2f };

    printf("[SIMULATOR] Sensor thread running at 20 Hz.\n");

    while (g_keep_running) {
        /* --- handle reset / new scenario requests from the HTTP thread --- */
        if (g_state.reset_requested) {
            g_state.reset_requested = false;
            g_has_pending = false;
            g_state.post_capture_remaining = 0;
            g_minor_active = ANOMALY_NONE;
            edge_filter_init(&g_filter);
        }
        if (g_state.scenario_epoch != last_epoch) {
            last_epoch = g_state.scenario_epoch;
            scenario_tick = 0;
            approach_done = false;
        }

        TelemetryPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.timestamp_ms = now_ms();
        pkt.accel_z = 1.0f;

        switch (g_state.current_scenario) {

        case 1: /* ---------------- SEVERE FRONTAL COLLISION ---------------- */
            if (!approach_done) {
                if (approach_speed(&speed, &pkt, 50.0f)) break;
                approach_done = true;
            }
            scenario_tick++;
            if (scenario_tick <= 3) {
                int i = scenario_tick - 1;
                pkt.accel_x = crash_ax[i] + jitter(0.2f);
                pkt.accel_y = crash_ay[i] + jitter(0.1f);
                pkt.accel_z = crash_az[i];
                speed = fmaxf(0.0f, speed + pkt.accel_x * 9.81f * 0.05f * 3.6f);
                pitch = -6.0f - 2.0f * (float)scenario_tick;
                roll  = 4.0f * (float)scenario_tick;
                pkt.brake_pressure = 100.0f;
            } else {
                speed = 0.0f;                  /* post-crash: vehicle stopped */
                pkt.accel_x = jitter(0.02f);
                pkt.accel_y = jitter(0.02f);
                pkt.accel_z = 1.0f;
                pitch = -2.0f; roll = 6.0f;
                pkt.brake_pressure = 0.0f;
            }
            rpm = speed > 0 ? fmaxf(800.0f, speed * 42.0f) : 0.0f;
            break;

        case 2: /* ---------------- ROLLOVER ---------------- */
            if (!approach_done) {
                if (approach_speed(&speed, &pkt, 50.0f)) break;
                approach_done = true;
            }
            scenario_tick++;
            if (scenario_tick <= 6) {
                roll += 15.0f;
                pkt.accel_x = -1.2f + jitter(0.1f);
                pkt.accel_y = 3.2f + jitter(0.2f);
                pkt.accel_z = 0.4f;
                speed = fmaxf(0.0f, speed - 8.0f);
            } else {
                roll = 90.0f;                  /* resting on its side */
                speed = 0.0f;
                pkt.accel_x = 0.0f;
                pkt.accel_y = 0.98f;           /* gravity now acts sideways */
                pkt.accel_z = 0.05f;
            }
            rpm = speed > 0 ? speed * 42.0f : 0.0f;
            break;

        case 3: /* ---------------- HARSH BRAKING ---------------- */
            if (!approach_done) {
                if (approach_speed(&speed, &pkt, 50.0f)) break;
                approach_done = true;
            }
            scenario_tick++;
            if (scenario_tick <= 25 && speed > 8.0f) {
                pkt.accel_x = -0.92f + jitter(0.04f);
                pkt.accel_y = jitter(0.03f);
                speed = fmaxf(0.0f, speed + pkt.accel_x * 9.81f * 0.05f * 3.6f);
                pitch = -3.5f;
                pkt.brake_pressure = 92.0f;
                rpm = fmaxf(900.0f, speed * 42.0f);
            } else {
                g_state.current_scenario = 0;  /* back to normal driving */
            }
            break;

        case 4: /* ---------------- ENGINE OVERHEAT ---------------- */
            temp = fminf(temp + 0.5f, 132.0f);
            if (speed < 42.0f) speed += 0.8f;
            pkt.accel_x = jitter(0.05f);
            pkt.accel_y = jitter(0.05f);
            pkt.accel_z = 1.0f + jitter(0.02f);
            rpm = 3400.0f + (float)(rand() % 400);
            break;

        case 0:
        default: /* ---------------- NORMAL CRUISING ---------------- */
            if (speed < 42.0f) {
                speed += 0.8f;                 /* smooth pull-away (~0.45 G) */
                pkt.accel_x = 0.45f + jitter(0.03f);
            } else {
                float drift = jitter(1.0f);
                speed = fminf(68.0f, fmaxf(42.0f, speed + drift));
                pkt.accel_x = drift / 3.6f / 0.05f / 9.81f * 0.5f + jitter(0.03f);
            }
            pkt.accel_y = jitter(0.08f);
            pkt.accel_z = 1.0f + jitter(0.03f);
            rpm   = fmaxf(800.0f, speed * 42.0f);
            roll  = jitter(1.0f);
            pitch = 1.0f + jitter(0.3f);
            temp += (90.0f - temp) * 0.03f + jitter(0.1f);   /* cool back down to ~90 C */
            break;
        }

        /* Integrate GPS position from speed + heading */
        if (speed > 0.1f) {
            double dist_m = (speed / 3.6) * (SAMPLE_PERIOD_MS / 1000.0);
            lat += (dist_m * cos(yaw * (M_PI / 180.0))) / 111139.0;
            lon += (dist_m * sin(yaw * (M_PI / 180.0))) / (111139.0 * cos(lat * (M_PI / 180.0)));
            yaw += jitter(0.6f);
            if (yaw >= 360.0f) yaw -= 360.0f;
            if (yaw < 0.0f) yaw += 360.0f;
        }

        pkt.speed_kmh     = speed;
        pkt.rpm           = rpm;
        pkt.gyro_pitch    = pitch;
        pkt.gyro_roll     = roll;
        pkt.gyro_yaw      = yaw;
        pkt.engine_temp_c = temp;
        pkt.latitude      = lat;
        pkt.longitude     = lon;

        /* ---------------- EDGE FILTER (local classification) ---------------- */
        g_state.current_inference_time_us = edge_filter_process(&g_filter, &pkt);
        g_state.total_packets_processed++;

        /* ---------------- CRITICAL EVENT: instant alert ---------------- */
        if (edge_filter_is_emergency(&pkt) && !g_state.is_frozen && !g_has_pending) {
            build_incident(&g_pending, &pkt, last_speed);
            g_has_pending = true;
            g_pending_trigger_offset = 0;
            g_state.post_capture_remaining = POST_TRIGGER_PACKETS;

            printf("\n==================================================================\n");
            printf("[EDGE ALERT] %s | %.2f G | score %.2f | classified in %.2f us\n",
                   anomaly_type_to_string(pkt.anomaly_type), pkt.total_g,
                   pkt.anomaly_score, g_state.current_inference_time_us);
            printf("[LOCALIZATION] Nearest: %s (%.2f km) %s\n",
                   g_pending.nearest_station.name, g_pending.nearest_station.distance_km,
                   g_pending.nearest_station.phone);
            printf("[RING BUFFER] Recording %d post-impact packets before freezing...\n",
                   POST_TRIGGER_PACKETS);
            write_description(&g_pending);
            http_server_broadcast_alert(&g_pending);   /* zero-wait alert to dashboard */
        }

        /* Track peak values while the post-impact window is being recorded */
        if (g_has_pending) {
            if (pkt.total_g > g_pending.peak_g) g_pending.peak_g = pkt.total_g;
            if (fabsf(pkt.gyro_roll) > fabsf(g_pending.roll_angle)) g_pending.roll_angle = pkt.gyro_roll;
        }

        /* ---------------- RING BUFFER: O(1) enqueue ---------------- */
        if (!g_state.is_frozen) {
            cq_enqueue(&g_queue, &pkt);
        }

        if (g_has_pending) {
            g_state.post_capture_remaining--;
            if (g_state.post_capture_remaining <= 0) {
                cq_freeze(&g_queue);             /* lock the black box */
                g_state.is_frozen = true;
                printf("[RING BUFFER] FROZEN. Snapshotting into linked list...\n");
                commit_incident(&g_pending, g_pending_trigger_offset);
                printf("==================================================================\n\n");
                g_has_pending = false;
                g_state.post_capture_remaining = 0;
            } else {
                g_pending_trigger_offset++;
            }
        } else if (!g_state.is_frozen) {
            /* ---------- Non-critical anomalies: log a node, no freeze ---------- */
            if (pkt.anomaly_type == ANOMALY_HARSH_BRAKE || pkt.anomaly_type == ANOMALY_THERMAL_RUNAWAY) {
                if (g_minor_active != pkt.anomaly_type) {   /* one node per event */
                    g_minor_active = pkt.anomaly_type;
                    build_incident(&g_minor, &pkt, pkt.anomaly_type == ANOMALY_HARSH_BRAKE ? last_speed : speed);
                    printf("[EDGE WARNING] %s detected (severity %s)\n",
                           anomaly_type_to_string(pkt.anomaly_type), severity_to_string(pkt.severity));
                    write_description(&g_minor);
                    http_server_broadcast_alert(&g_minor);
                    commit_incident(&g_minor, 0);
                }
            } else if (pkt.anomaly_type == ANOMALY_NONE) {
                g_minor_active = ANOMALY_NONE;
            }
        }

        last_speed = speed;
        http_server_broadcast_telemetry(&pkt, &g_state, &g_queue);
        usleep(SAMPLE_PERIOD_MS * 1000);
    }
    return NULL;
}

static void ensure_parent_dir(const char *path) {
    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            perror("mkdir data directory");
        }
    }
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_SERVER_PORT;
    const char *web_root = "./web";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)      port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) web_root = argv[++i];
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) g_blackbox_path = argv[++i];
        else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-p port] [-w web_root] [-b blackbox_file]\n", argv[0]);
            return 0;
        }
    }

    srand((unsigned)time(NULL));

    printf("==================================================================\n");
    printf("        TELEMETRY & ANOMALY RECONSTRUCTION ENGINE (C99)           \n");
    printf("  Data structures: struct | circular queue | doubly linked list   \n");
    printf("==================================================================\n");

    localization_init();
    edge_filter_init(&g_filter);

    if (!cq_init(&g_queue, TELEMETRY_WINDOW_SIZE)) {
        fprintf(stderr, "Failed to initialise circular queue\n");
        return 1;
    }
    if (!list_init(&g_incidents)) {
        fprintf(stderr, "Failed to initialise incident list\n");
        cq_free(&g_queue);
        return 1;
    }

    /* Data resilience: rebuild the linked list from the black-box file */
    ensure_parent_dir(g_blackbox_path);
    int restored = list_load_from_file(&g_incidents, g_blackbox_path);
    if (restored > 0) {
        printf("[BLACK BOX] Restored %d incident(s) from %s\n", restored, g_blackbox_path);
    } else if (restored < 0) {
        printf("[BLACK BOX] %s is incompatible/corrupt - starting fresh.\n", g_blackbox_path);
        list_clear_file(g_blackbox_path);
    } else {
        printf("[BLACK BOX] No previous incidents (%s)\n", g_blackbox_path);
    }

    memset(&g_state, 0, sizeof(g_state));
    strncpy(g_state.vehicle_id, "MH-12-DS-2026", sizeof(g_state.vehicle_id) - 1);
    g_state.is_running = true;
    if (restored > 0) g_state.latest_incident_id = g_incidents.tail->data.incident_id;

    printf("[MEMORY] sizeof(TelemetryPacket)=%zu B | ring buffer=%zu B | sizeof(IncidentNode)=%zu B\n",
           sizeof(TelemetryPacket), sizeof(TelemetryPacket) * TELEMETRY_WINDOW_SIZE, sizeof(IncidentNode));

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    g_server_config.port          = port;
    g_server_config.web_root      = web_root;
    g_server_config.blackbox_path = g_blackbox_path;
    g_server_config.queue         = &g_queue;
    g_server_config.incidents     = &g_incidents;
    g_server_config.vehicle_state = &g_state;

    if (!http_server_start(&g_server_config)) {
        fprintf(stderr, "Failed to start HTTP server on port %d. Try: ./bin/telemetry_server -p 8081\n", port);
        cq_free(&g_queue);
        list_free(&g_incidents);
        return 1;
    }

    printf("[SERVER] Dashboard:  http://localhost:%d\n", port);

    if (pthread_create(&g_sim_thread, NULL, simulation_worker, NULL) != 0) {
        fprintf(stderr, "Failed to start simulation thread\n");
        http_server_stop();
        cq_free(&g_queue);
        list_free(&g_incidents);
        return 1;
    }

    printf("[STATUS] Running. Press Ctrl+C to stop.\n\n");

    while (g_keep_running) sleep(1);

    printf("\n[SHUTDOWN] Stopping sensor thread...\n");
    pthread_join(g_sim_thread, NULL);
    printf("[SHUTDOWN] Stopping HTTP server...\n");
    http_server_stop();
    printf("[SHUTDOWN] Freeing ring buffer and %d linked-list node(s)...\n", list_size(&g_incidents));
    cq_free(&g_queue);
    list_free(&g_incidents);
    printf("[SHUTDOWN] Clean exit. Incidents remain saved in %s\n", g_blackbox_path);
    return 0;
}
