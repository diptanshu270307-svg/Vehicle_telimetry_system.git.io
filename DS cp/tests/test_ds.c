#include "telemetry.h"
#include "circular_queue.h"
#include "linked_list.h"
#include "edge_filter.h"
#include "localization.h"
#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/socket.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
    } \
} while(0)

static void test_circular_queue_basic(void) {
    printf("\n--- Test: Circular Queue Operations ---\n");
    CircularQueue q;
    bool ok = cq_init(&q, 5);
    TEST_ASSERT(ok, "Circular Queue initializes with capacity 5");

    TelemetryPacket p1, p2, p3;
    memset(&p1, 0, sizeof(p1)); p1.timestamp_ms = 100; p1.speed_kmh = 30.0f;
    memset(&p2, 0, sizeof(p2)); p2.timestamp_ms = 200; p2.speed_kmh = 40.0f;
    memset(&p3, 0, sizeof(p3)); p3.timestamp_ms = 300; p3.speed_kmh = 50.0f;

    cq_enqueue(&q, &p1);
    cq_enqueue(&q, &p2);
    cq_enqueue(&q, &p3);

    int head, tail, count, cap;
    bool frozen;
    cq_get_stats(&q, &head, &tail, &count, &cap, &frozen);
    TEST_ASSERT(count == 3 && head == 3 && tail == 0, "Queue enqueues 3 items without wrap");

    TelemetryPacket latest;
    cq_peek_latest(&q, &latest);
    TEST_ASSERT(latest.timestamp_ms == 300, "Peek latest returns newest packet");

    TelemetryPacket out;
    cq_dequeue(&q, &out);
    TEST_ASSERT(out.timestamp_ms == 100, "Dequeue returns oldest packet (FIFO)");

    cq_free(&q);
}

static void test_circular_queue_overflow_and_freeze(void) {
    printf("\n--- Test: Circular Queue Wrap-Around Overwrite & Freeze ---\n");
    CircularQueue q;
    cq_init(&q, 3);

    TelemetryPacket pkts[5];
    for (int i = 0; i < 5; i++) {
        memset(&pkts[i], 0, sizeof(pkts[i]));
        pkts[i].timestamp_ms = 1000 + i;
        pkts[i].speed_kmh = 10.0f * i;
        cq_enqueue(&q, &pkts[i]);
    }

    int head, tail, count, cap;
    bool frozen;
    cq_get_stats(&q, &head, &tail, &count, &cap, &frozen);
    TEST_ASSERT(count == 3, "Queue count capped at capacity 3");

    /* When 5 items inserted into capacity 3, items 0 and 1 are overwritten.
       Remaining in buffer: 2, 3, 4 */
    TelemetryPacket ordered[5];
    int n = cq_get_all_ordered(&q, ordered, 5);
    TEST_ASSERT(n == 3, "Chronological snapshot returns exactly 3 packets");
    TEST_ASSERT(ordered[0].timestamp_ms == 1002, "Oldest packet is item 2 (1002ms)");
    TEST_ASSERT(ordered[1].timestamp_ms == 1003, "Middle packet is item 3 (1003ms)");
    TEST_ASSERT(ordered[2].timestamp_ms == 1004, "Newest packet is item 4 (1004ms)");

    /* Test Freeze */
    cq_freeze(&q);
    TEST_ASSERT(cq_is_frozen(&q), "Queue is marked as frozen");

    TelemetryPacket p_fail;
    memset(&p_fail, 0, sizeof(p_fail));
    p_fail.timestamp_ms = 9999;
    bool enq_result = cq_enqueue(&q, &p_fail);
    TEST_ASSERT(!enq_result, "Enqueue rejected while queue is frozen (preserves black-box)");

    cq_unfreeze(&q);
    TEST_ASSERT(!cq_is_frozen(&q), "Queue successfully unfreezes");

    cq_free(&q);
}

static void test_linked_list(void) {
    printf("\n--- Test: Incident Linked List ---\n");
    IncidentList list;
    bool ok = list_init(&list);
    TEST_ASSERT(ok, "Incident List initializes successfully");
    TEST_ASSERT(list_size(&list) == 0, "Initial list size is 0");

    IncidentRecord r1, r2;
    memset(&r1, 0, sizeof(r1));
    r1.timestamp_ms = 50000;
    r1.type = ANOMALY_CRASH_COLLISION;
    r1.severity = SEVERITY_CRITICAL;
    r1.peak_g = 6.4f;
    strncpy(r1.description, "Head-on collision", sizeof(r1.description));

    memset(&r2, 0, sizeof(r2));
    r2.timestamp_ms = 60000;
    r2.type = ANOMALY_ROLLOVER;
    r2.severity = SEVERITY_CRITICAL;
    r2.peak_g = 3.8f;
    strncpy(r2.description, "Rollover on highway", sizeof(r2.description));

    int id1 = list_insert_incident(&list, r1);
    int id2 = list_insert_incident(&list, r2);

    TEST_ASSERT(id1 == 1, "First incident assigned ID 1");
    TEST_ASSERT(id2 == 2, "Second incident assigned ID 2");
    TEST_ASSERT(list_size(&list) == 2, "List size is 2 after insertions");

    IncidentRecord fetched;
    bool found = list_get_incident(&list, 2, &fetched);
    TEST_ASSERT(found && fetched.type == ANOMALY_ROLLOVER, "Lookup by ID retrieves correct incident");

    char json_buf[4096];
    int json_len = list_to_summary_json(&list, json_buf, sizeof(json_buf));
    TEST_ASSERT(json_len > 0 && strstr(json_buf, "CRASH_COLLISION") && strstr(json_buf, "ROLLOVER"),
                "Summary JSON correctly serializes all incident nodes");

    list_free(&list);
    TEST_ASSERT(list.head == NULL && list_size(&list) == 0, "List free cleans up all nodes");
}

static void test_edge_filter_tinyml(void) {
    printf("\n--- Test: Edge Filter & TinyML Inference ---\n");
    EdgeFilterModel model;
    edge_filter_init(&model);

    /* 1. Normal packet */
    TelemetryPacket p_norm;
    memset(&p_norm, 0, sizeof(p_norm));
    p_norm.timestamp_ms = 1000;
    p_norm.speed_kmh = 50.0f;
    p_norm.accel_x = 0.05f;
    p_norm.accel_y = 0.02f;
    p_norm.accel_z = 1.0f;
    p_norm.engine_temp_c = 90.0f;

    float lat_us = edge_filter_process(&model, &p_norm);
    TEST_ASSERT(p_norm.anomaly_type == ANOMALY_NONE, "Normal driving classified as ANOMALY_NONE");
    TEST_ASSERT(p_norm.severity == SEVERITY_NORMAL, "Normal driving severity is SEVERITY_NORMAL");
    TEST_ASSERT(lat_us >= 0.0f && lat_us < 500.0f, "Inference latency is ultra-low (< 500 us)");

    /* 2. Crash packet */
    TelemetryPacket p_crash;
    memset(&p_crash, 0, sizeof(p_crash));
    p_crash.timestamp_ms = 1050;
    p_crash.speed_kmh = 10.0f;
    p_crash.accel_x = -5.5f;
    p_crash.accel_y = 2.5f;
    p_crash.accel_z = 1.5f;
    p_crash.engine_temp_c = 91.0f;

    edge_filter_process(&model, &p_crash);
    TEST_ASSERT(p_crash.anomaly_type == ANOMALY_CRASH_COLLISION, "Crash shock classified as ANOMALY_CRASH_COLLISION");
    TEST_ASSERT(p_crash.severity == SEVERITY_CRITICAL || p_crash.severity == SEVERITY_HIGH, "Crash severity is CRITICAL/HIGH");
    TEST_ASSERT(p_crash.anomaly_score >= 0.90f, "TinyML anomaly score > 0.90");
    TEST_ASSERT(edge_filter_is_emergency(&p_crash), "Identified as emergency requiring freeze");

    /* 3. Rollover packet */
    TelemetryPacket p_roll;
    memset(&p_roll, 0, sizeof(p_roll));
    p_roll.timestamp_ms = 1100;
    p_roll.speed_kmh = 20.0f;
    p_roll.accel_x = 0.0f;
    p_roll.accel_y = 1.2f;
    p_roll.accel_z = 0.2f;
    p_roll.gyro_roll = 65.0f;
    p_roll.engine_temp_c = 91.0f;

    edge_filter_process(&model, &p_roll);
    TEST_ASSERT(p_roll.anomaly_type == ANOMALY_ROLLOVER, "Roll angle > 50 deg classified as ANOMALY_ROLLOVER");
}

static void test_localization(void) {
    printf("\n--- Test: Instant Localization & Nearest Facility ---\n");
    localization_init();

    /* Known distance test: Pune station (18.5284, 73.8743) to Ruby Hall (18.5328, 73.8772) ~ 0.58 km */
    double dist = calculate_haversine_distance(18.5284, 73.8743, 18.5328, 73.8772);
    TEST_ASSERT(dist > 0.4 && dist < 0.8, "Haversine distance accurate within expected bounds");

    EmergencyStation nearest;
    bool found = localization_find_nearest(18.5320, 73.8765, &nearest);
    TEST_ASSERT(found, "Emergency station found for given coordinates");
    TEST_ASSERT(nearest.distance_km < 1.0, "Nearest facility resolved within < 1.0 km");
    TEST_ASSERT(strlen(nearest.name) > 0, "Nearest station contains name");
    TEST_ASSERT(strlen(nearest.phone) > 0, "Nearest station contains contact phone number");
}

static void* test_handler_worker(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle_client_request(fd);
    return NULL;
}

static void dispatch_test_request(const char *req, char *resp_out, size_t max_resp) {
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    int sz = 65536;
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));

    write(fds[0], req, strlen(req));

    pthread_t th;
    pthread_create(&th, NULL, test_handler_worker, (void*)(intptr_t)fds[1]);

    ssize_t n = 0;
    size_t total = 0;
    while ((n = read(fds[0], resp_out + total, max_resp - 1 - total)) > 0) {
        total += n;
        if (total >= max_resp - 1) break;
    }
    resp_out[total] = '\0';
    close(fds[0]);
    pthread_join(th, NULL);
}

static void test_http_endpoints(void) {
    printf("\n--- Test: HTTP Server Endpoints & In-Process Request Dispatch ---\n");
    CircularQueue q;
    cq_init(&q, 10);
    IncidentList list;
    list_init(&list);
    VehicleState state;
    memset(&state, 0, sizeof(state));
    strncpy(state.vehicle_id, "TEST-VEHICLE-1", sizeof(state.vehicle_id));
    state.is_running = true;

    HttpServerConfig cfg;
    cfg.port = 8080;
    cfg.web_root = "./web";
    cfg.blackbox_path = NULL;
    cfg.queue = &q;
    cfg.incidents = &list;
    cfg.vehicle_state = &state;
    cfg.is_running = true;
    http_server_set_config(&cfg);

    char resp[32768];

    /* 1. Test GET /api/status */
    dispatch_test_request("GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n", resp, sizeof(resp));
    TEST_ASSERT(strstr(resp, "HTTP/1.1 200 OK") != NULL, "GET /api/status returns 200 OK");
    TEST_ASSERT(strstr(resp, "TEST-VEHICLE-1") != NULL, "Status response contains vehicle ID");
    TEST_ASSERT(strstr(resp, "memory_stats") != NULL, "Status response contains memory stats");

    /* 2. Test GET /api/stations */
    dispatch_test_request("GET /api/stations HTTP/1.1\r\nHost: localhost\r\n\r\n", resp, sizeof(resp));
    TEST_ASSERT(strstr(resp, "HTTP/1.1 200 OK") != NULL, "GET /api/stations returns 200 OK");
    TEST_ASSERT(strstr(resp, "Ruby Hall Clinic") != NULL, "Stations JSON contains emergency facility");

    /* 3. Test Static File: GET /index.html */
    dispatch_test_request("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n", resp, sizeof(resp));
    TEST_ASSERT(strstr(resp, "HTTP/1.1 200 OK") != NULL, "GET /index.html returns 200 OK");
    TEST_ASSERT(strstr(resp, "Content-Type: text/html") != NULL, "index.html has text/html content-type");
    TEST_ASSERT(strstr(resp, "TELEMETRY & ANOMALY RECONSTRUCTION ENGINE") != NULL, "Served HTML contains project title");

    /* 4. Test POST /api/simulate */
    dispatch_test_request("POST /api/simulate?scenario=collision HTTP/1.1\r\nHost: localhost\r\n\r\n", resp, sizeof(resp));
    TEST_ASSERT(strstr(resp, "HTTP/1.1 200 OK") != NULL, "POST /api/simulate returns 200 OK");
    TEST_ASSERT(state.current_scenario == 1, "Scenario successfully updated to 1 (Collision)");

    cq_free(&q);
    list_free(&list);
}

static void test_persistence(void) {
    printf("\n--- Test: Black-Box Persistence (Data Resilience) ---\n");
    const char *path = "./bin/test_blackbox.bin";
    list_clear_file(path);

    IncidentRecord r;
    memset(&r, 0, sizeof(r));
    r.type = ANOMALY_CRASH_COLLISION;
    r.peak_g = 14.2f;
    r.packet_count = 3;
    r.trigger_index = 1;
    r.packets[1].total_g = 14.2f;
    strncpy(r.description, "persist test", sizeof(r.description) - 1);

    IncidentList a;
    list_init(&a);
    r.incident_id = list_insert_incident(&a, r);
    TEST_ASSERT(list_append_to_file(&r, path), "Incident #1 appended to black-box file");
    r.incident_id = list_insert_incident(&a, r);
    TEST_ASSERT(list_append_to_file(&r, path), "Incident #2 appended to black-box file");
    list_free(&a);

    /* Simulate a restart: brand new list rebuilt from disk */
    IncidentList b;
    list_init(&b);
    int loaded = list_load_from_file(&b, path);
    TEST_ASSERT(loaded == 2, "Both nodes restored from disk after restart");
    TEST_ASSERT(b.head && b.head->next == b.tail && b.tail->prev == b.head, "Restored list prev/next links are correct");
    TEST_ASSERT(b.tail && b.tail->data.packets[1].total_g > 14.0f, "Black-box packet data survived the round trip");

    int new_id = list_insert_incident(&b, r);
    TEST_ASSERT(new_id == 3, "Next incident ID continues after restored IDs (no duplicates)");
    list_free(&b);

    TEST_ASSERT(list_clear_file(path), "Black-box file cleared");
}

int main(void) {
    printf("====================================================\n");
    printf("  RUNNING TELEMETRY ENGINE DATA STRUCTURES TEST SUITE\n");
    printf("====================================================\n");

    test_circular_queue_basic();
    test_circular_queue_overflow_and_freeze();
    test_linked_list();
    test_edge_filter_tinyml();
    test_localization();
    test_http_endpoints();
    test_persistence();

    printf("\n====================================================\n");
    printf("  TEST RESULTS: %d / %d PASSED (%.1f%%)\n",
           tests_passed, tests_run, (float)tests_passed / tests_run * 100.0f);
    printf("====================================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
