#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "telemetry.h"
#include "circular_queue.h"
#include "linked_list.h"
#include <stdbool.h>

typedef struct {
    int port;
    const char *web_root;
    const char *blackbox_path;   /* Persistent incident file (may be NULL) */
    CircularQueue *queue;
    IncidentList *incidents;
    VehicleState *vehicle_state;
    bool is_running;
} HttpServerConfig;

/* Start the built-in C HTTP & SSE server in a background thread */
bool http_server_start(HttpServerConfig *config);

/* Stop the server */
void http_server_stop(void);

/* Broadcast a live telemetry packet to all connected SSE browser clients */
void http_server_broadcast_telemetry(const TelemetryPacket *packet, const VehicleState *state, const CircularQueue *queue);

/* Instant alert: sent the moment the edge filter fires (before the node is committed) */
void http_server_broadcast_alert(const IncidentRecord *incident);

/* Sent once the incident node has been committed to the linked list + disk */
void http_server_broadcast_incident(const IncidentRecord *incident);

/* Set server config for direct request handling (used by unit tests) */
void http_server_set_config(HttpServerConfig *config);

/* Handle an individual HTTP client request */
void handle_client_request(int client_fd);

#endif /* HTTP_SERVER_H */
