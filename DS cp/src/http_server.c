/*
 * Minimal HTTP/1.1 + Server-Sent-Events server written with POSIX sockets.
 * - One detached thread per incoming connection (a stalled browser socket
 *   can never block other requests).
 * - SSE clients are kept in an array and receive telemetry at 20 Hz.
 */
#include "http_server.h"
#include "localization.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX_SSE_CLIENTS 32
#define BUFFER_SIZE 8192
#define RECONSTRUCT_BUFFER_SIZE 65536

static HttpServerConfig *g_config = NULL;
static int g_server_fd = -1;
static pthread_t g_server_thread;
static volatile bool g_running = false;

static int g_sse_clients[MAX_SSE_CLIENTS];
static int g_sse_client_count = 0;
static pthread_mutex_t g_sse_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                   */
/* ------------------------------------------------------------------ */

static bool write_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static void set_socket_timeouts(int fd, int recv_ms, int send_ms) {
    struct timeval rt = { recv_ms / 1000, (recv_ms % 1000) * 1000 };
    struct timeval st = { send_ms / 1000, (send_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

/* ------------------------------------------------------------------ */
/* SSE client management                                               */
/* ------------------------------------------------------------------ */

static void add_sse_client(int fd) {
    pthread_mutex_lock(&g_sse_lock);
    if (g_sse_client_count < MAX_SSE_CLIENTS) {
        g_sse_clients[g_sse_client_count++] = fd;
    } else {
        close(fd);
    }
    pthread_mutex_unlock(&g_sse_lock);
}

/* Caller must hold g_sse_lock */
static void remove_sse_client_at(int idx) {
    if (idx >= 0 && idx < g_sse_client_count) {
        close(g_sse_clients[idx]);
        for (int i = idx; i < g_sse_client_count - 1; i++) {
            g_sse_clients[i] = g_sse_clients[i + 1];
        }
        g_sse_client_count--;
    }
}

static void sse_broadcast(const char *msg, int len) {
    pthread_mutex_lock(&g_sse_lock);
    for (int i = 0; i < g_sse_client_count; ) {
        if (!write_all(g_sse_clients[i], msg, (size_t)len)) {
            remove_sse_client_at(i);   /* browser tab closed or stalled */
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&g_sse_lock);
}

void http_server_broadcast_telemetry(const TelemetryPacket *packet, const VehicleState *state, const CircularQueue *queue) {
    if (!packet || !state || !queue) return;

    int q_head, q_tail, q_count, q_cap;
    bool q_frozen;
    cq_get_stats((CircularQueue *)queue, &q_head, &q_tail, &q_count, &q_cap, &q_frozen);

    /* Live localization: nearest emergency facility for the CURRENT position */
    EmergencyStation nearest;
    memset(&nearest, 0, sizeof(nearest));
    localization_find_nearest(packet->latitude, packet->longitude, &nearest);

    char msg[1600];
    int len = snprintf(msg, sizeof(msg),
        "data: {\"event\":\"telemetry\",\"t\":%llu,\"speed\":%.1f,\"rpm\":%.0f,"
        "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,\"g\":%.2f,"
        "\"pitch\":%.1f,\"roll\":%.1f,\"yaw\":%.1f,\"brake\":%.1f,\"temp\":%.1f,"
        "\"lat\":%.6f,\"lon\":%.6f,\"score\":%.3f,\"type\":\"%s\",\"severity\":\"%s\","
        "\"nearest\":{\"name\":\"%s\",\"dist\":%.2f,\"phone\":\"%s\"},"
        "\"queue\":{\"head\":%d,\"tail\":%d,\"count\":%d,\"capacity\":%d,\"frozen\":%s},"
        "\"state\":{\"scenario\":%d,\"frozen\":%s,\"capturing\":%d,\"latest_incident\":%d,"
        "\"total_pkts\":%llu,\"anomalies\":%llu,\"lat_us\":%.2f}}\n\n",
        (unsigned long long)packet->timestamp_ms,
        packet->speed_kmh, packet->rpm,
        packet->accel_x, packet->accel_y, packet->accel_z, packet->total_g,
        packet->gyro_pitch, packet->gyro_roll, packet->gyro_yaw,
        packet->brake_pressure, packet->engine_temp_c,
        packet->latitude, packet->longitude,
        packet->anomaly_score,
        anomaly_type_to_string(packet->anomaly_type),
        severity_to_string(packet->severity),
        nearest.name, nearest.distance_km, nearest.phone,
        q_head, q_tail, q_count, q_cap, q_frozen ? "true" : "false",
        state->current_scenario, state->is_frozen ? "true" : "false",
        state->post_capture_remaining, state->latest_incident_id,
        (unsigned long long)state->total_packets_processed,
        (unsigned long long)state->total_anomalies_detected,
        state->current_inference_time_us);

    if (len > 0 && len < (int)sizeof(msg)) sse_broadcast(msg, len);
}

static void broadcast_incident_event(const char *event, const IncidentRecord *incident) {
    if (!incident) return;
    char msg[1024];
    int len = snprintf(msg, sizeof(msg),
        "data: {\"event\":\"%s\",\"id\":%d,\"timestamp\":%llu,\"type\":\"%s\",\"severity\":\"%s\","
        "\"peak_g\":%.2f,\"impact_speed\":%.1f,\"roll_angle\":%.1f,"
        "\"lat\":%.6f,\"lon\":%.6f,\"hospital\":\"%s\",\"hospital_dist\":%.2f,\"phone\":\"%s\",\"desc\":\"%s\"}\n\n",
        event,
        incident->incident_id,
        (unsigned long long)incident->timestamp_ms,
        anomaly_type_to_string(incident->type),
        severity_to_string(incident->severity),
        incident->peak_g,
        incident->impact_speed_kmh,
        incident->roll_angle,
        incident->latitude,
        incident->longitude,
        incident->nearest_station.name,
        incident->nearest_station.distance_km,
        incident->nearest_station.phone,
        incident->description);
    if (len > 0 && len < (int)sizeof(msg)) sse_broadcast(msg, len);
}

void http_server_broadcast_alert(const IncidentRecord *incident) {
    broadcast_incident_event("alert", incident);
}

void http_server_broadcast_incident(const IncidentRecord *incident) {
    broadcast_incident_event("incident", incident);
}

/* ------------------------------------------------------------------ */
/* HTTP responses                                                      */
/* ------------------------------------------------------------------ */

static const char* get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(ext, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    return "text/plain";
}

static void send_response(int fd, int code, const char *status, const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: Telemetry-Edge-C/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "\r\n",
        code, status, content_type, body_len);
    write_all(fd, header, (size_t)hlen);
    if (body && body_len > 0) write_all(fd, body, body_len);
}

static void send_json(int fd, const char *json, size_t len) {
    send_response(fd, 200, "OK", "application/json", json, len);
}

static void serve_file(int fd, const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        const char *not_found = "{\"error\":\"File not found\"}";
        send_response(fd, 404, "Not Found", "application/json", not_found, strlen(not_found));
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return; }

    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        const char *err = "{\"error\":\"Memory allocation failed\"}";
        send_response(fd, 500, "Internal Error", "application/json", err, strlen(err));
        return;
    }

    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);

    send_response(fd, 200, "OK", get_mime_type(filepath), data, got);
    free(data);
}

/* Extract value of ?key=VALUE from a path into out (empty string if absent) */
static void query_param(const char *path, const char *key, char *out, size_t out_size) {
    out[0] = '\0';
    const char *q = strchr(path, '?');
    if (!q) return;
    size_t klen = strlen(key);
    const char *p = q + 1;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < out_size) out[i++] = *p++;
            out[i] = '\0';
            return;
        }
        const char *amp = strchr(p, '&');
        if (!amp) return;
        p = amp + 1;
    }
}

void http_server_set_config(HttpServerConfig *config) {
    g_config = config;
}

/* ------------------------------------------------------------------ */
/* Request router                                                      */
/* ------------------------------------------------------------------ */

void handle_client_request(int client_fd) {
    char request[BUFFER_SIZE];
    ssize_t bytes_read = read(client_fd, request, sizeof(request) - 1);
    if (bytes_read <= 0) {      /* timeout or empty speculative connection */
        close(client_fd);
        return;
    }
    request[bytes_read] = '\0';

    char method[16], path[256], proto[16];
    if (sscanf(request, "%15s %255s %15s", method, path, proto) < 2) {
        close(client_fd);
        return;
    }

    VehicleState *vs = g_config->vehicle_state;

    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client_fd, 200, "OK", "text/plain", "", 0);
        close(client_fd);
        return;
    }

    /* ---- SSE live stream ---- */
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/events", 11) == 0) {
        const char *sse_headers =
            "HTTP/1.1 200 OK\r\n"
            "Server: Telemetry-Edge-C/1.0\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        write_all(client_fd, sse_headers, strlen(sse_headers));
        add_sse_client(client_fd);
        return; /* keep socket open */
    }

    /* ---- GET /api/status ---- */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        char json[1200];
        int q_head, q_tail, q_count, q_cap;
        bool q_frozen;
        cq_get_stats(g_config->queue, &q_head, &q_tail, &q_count, &q_cap, &q_frozen);
        int inc_count = list_size(g_config->incidents);

        int len = snprintf(json, sizeof(json),
            "{\"vehicle_id\":\"%s\",\"running\":%s,\"scenario\":%d,\"frozen\":%s,"
            "\"packets_processed\":%llu,\"anomalies_detected\":%llu,\"latency_us\":%.2f,"
            "\"queue\":{\"head\":%d,\"tail\":%d,\"count\":%d,\"capacity\":%d,\"frozen\":%s},"
            "\"incidents_count\":%d,\"latest_incident\":%d,"
            "\"memory_stats\":{\"packet_size\":%zu,\"queue_buffer_bytes\":%zu,"
            "\"incident_record_bytes\":%zu,\"node_bytes\":%zu,\"list_total_bytes\":%zu},"
            "\"blackbox_file\":\"%s\"}",
            vs->vehicle_id,
            vs->is_running ? "true" : "false",
            vs->current_scenario,
            vs->is_frozen ? "true" : "false",
            (unsigned long long)vs->total_packets_processed,
            (unsigned long long)vs->total_anomalies_detected,
            vs->current_inference_time_us,
            q_head, q_tail, q_count, q_cap, q_frozen ? "true" : "false",
            inc_count, vs->latest_incident_id,
            sizeof(TelemetryPacket),
            sizeof(TelemetryPacket) * (size_t)q_cap,
            sizeof(IncidentRecord),
            sizeof(IncidentNode),
            sizeof(IncidentNode) * (size_t)inc_count,
            g_config->blackbox_path ? g_config->blackbox_path : "");

        send_json(client_fd, json, (size_t)len);
        close(client_fd);
        return;
    }

    /* ---- GET /api/incidents ---- */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/incidents") == 0) {
        size_t cap = 65536;
        char *json = (char *)malloc(cap);
        if (json) {
            int len = list_to_summary_json(g_config->incidents, json, cap);
            send_json(client_fd, json, (size_t)len);
            free(json);
        } else {
            send_json(client_fd, "[]", 2);
        }
        close(client_fd);
        return;
    }

    /* ---- GET /api/reconstruct?id=N ---- */
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/reconstruct", 16) == 0) {
        char idbuf[16];
        query_param(path, "id", idbuf, sizeof(idbuf));
        int id = idbuf[0] ? atoi(idbuf) : vs->latest_incident_id;

        char *json = (char *)malloc(RECONSTRUCT_BUFFER_SIZE);
        if (json) {
            int len = list_incident_to_json(g_config->incidents, id, json, RECONSTRUCT_BUFFER_SIZE);
            send_json(client_fd, json, (size_t)len);
            free(json);
        } else {
            send_json(client_fd, "{}", 2);
        }
        close(client_fd);
        return;
    }

    /* ---- GET /api/stations ---- */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/stations") == 0) {
        char json[4096];
        int len = localization_stations_to_json(json, sizeof(json));
        send_json(client_fd, json, (size_t)len);
        close(client_fd);
        return;
    }

    /* ---- POST /api/simulate?scenario=... ---- */
    if (strcmp(method, "POST") == 0 && strncmp(path, "/api/simulate", 13) == 0) {
        char sc[32];
        query_param(path, "scenario", sc, sizeof(sc));

        int new_scenario = -1;
        if      (strcmp(sc, "normal") == 0)    new_scenario = 0;
        else if (strcmp(sc, "collision") == 0) new_scenario = 1;
        else if (strcmp(sc, "rollover") == 0)  new_scenario = 2;
        else if (strcmp(sc, "brake") == 0)     new_scenario = 3;
        else if (strcmp(sc, "overheat") == 0)  new_scenario = 4;

        if (new_scenario >= 0) {
            /* Triggering a new scenario = operator acknowledged the previous
               incident (already safely stored in the list), so resume recording. */
            vs->is_frozen = false;
            cq_unfreeze(g_config->queue);
            vs->current_scenario = new_scenario;
            vs->scenario_epoch++;
        } else if (strcmp(sc, "unfreeze") == 0) {
            vs->is_frozen = false;
            cq_unfreeze(g_config->queue);
        } else {
            const char *bad = "{\"error\":\"unknown scenario\"}";
            send_response(client_fd, 400, "Bad Request", "application/json", bad, strlen(bad));
            close(client_fd);
            return;
        }

        char buf[96];
        int len = snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"scenario\":%d,\"frozen\":%s}",
                           vs->current_scenario, vs->is_frozen ? "true" : "false");
        send_json(client_fd, buf, (size_t)len);
        close(client_fd);
        return;
    }

    /* ---- POST /api/reset ---- */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reset") == 0) {
        vs->reset_requested = true;          /* sim thread drops any pending capture */
        cq_clear(g_config->queue);
        list_clear(g_config->incidents);
        if (g_config->blackbox_path) list_clear_file(g_config->blackbox_path);
        vs->is_frozen = false;
        vs->current_scenario = 0;
        vs->scenario_epoch++;
        vs->latest_incident_id = 0;
        vs->total_packets_processed = 0;
        vs->total_anomalies_detected = 0;

        const char *resp = "{\"status\":\"reset_complete\"}";
        send_json(client_fd, resp, strlen(resp));
        close(client_fd);
        return;
    }

    /* ---- Static files ---- */
    if (strstr(path, "..")) {
        const char *denied = "{\"error\":\"Access Denied\"}";
        send_response(client_fd, 403, "Forbidden", "application/json", denied, strlen(denied));
        close(client_fd);
        return;
    }

    char clean_path[256];
    strncpy(clean_path, path, sizeof(clean_path) - 1);
    clean_path[sizeof(clean_path) - 1] = '\0';
    char *qmark = strchr(clean_path, '?');
    if (qmark) *qmark = '\0';

    char filepath[512];
    if (strcmp(clean_path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", g_config->web_root);
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", g_config->web_root, clean_path);
    }

    serve_file(client_fd, filepath);
    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Accept loop                                                         */
/* ------------------------------------------------------------------ */

static void* connection_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle_client_request(fd);
    return NULL;
}

static void* server_worker_thread(void *arg) {
    (void)arg;
    while (g_running) {
        /* select() with timeout so Ctrl+C shutdown is always responsive */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_server_fd, &rfds);
        struct timeval tv = { 0, 250000 };
        int r = select(g_server_fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        set_socket_timeouts(client_fd, 3000, 500);

        pthread_t th;
        if (pthread_create(&th, NULL, connection_thread, (void *)(intptr_t)client_fd) == 0) {
            pthread_detach(th);
        } else {
            handle_client_request(client_fd);
        }
    }
    return NULL;
}

bool http_server_start(HttpServerConfig *config) {
    if (!config) return false;
    g_config = config;

    signal(SIGPIPE, SIG_IGN);

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("socket failed");
        return false;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((uint16_t)config->port);

    if (bind(g_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed (is another copy already running on this port?)");
        close(g_server_fd);
        g_server_fd = -1;
        return false;
    }

    if (listen(g_server_fd, 32) < 0) {
        perror("listen failed");
        close(g_server_fd);
        g_server_fd = -1;
        return false;
    }

    g_running = true;
    config->is_running = true;

    if (pthread_create(&g_server_thread, NULL, server_worker_thread, NULL) != 0) {
        perror("pthread_create for server failed");
        close(g_server_fd);
        g_server_fd = -1;
        g_running = false;
        return false;
    }
    return true;
}

void http_server_stop(void) {
    if (!g_running) return;
    g_running = false;
    pthread_join(g_server_thread, NULL);

    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    pthread_mutex_lock(&g_sse_lock);
    for (int i = 0; i < g_sse_client_count; i++) close(g_sse_clients[i]);
    g_sse_client_count = 0;
    pthread_mutex_unlock(&g_sse_lock);
}
