#include "linked_list.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

const char* anomaly_type_to_string(AnomalyType type) {
    switch (type) {
        case ANOMALY_CRASH_COLLISION: return "CRASH_COLLISION";
        case ANOMALY_ROLLOVER:        return "ROLLOVER";
        case ANOMALY_HARSH_BRAKE:     return "HARSH_BRAKE";
        case ANOMALY_THERMAL_RUNAWAY: return "THERMAL_RUNAWAY";
        case ANOMALY_HIGH_SPEED_IMPACT: return "HIGH_SPEED_IMPACT";
        default:                      return "NORMAL";
    }
}

const char* severity_to_string(SeverityLevel severity) {
    switch (severity) {
        case SEVERITY_LOW:      return "LOW";
        case SEVERITY_MEDIUM:   return "MEDIUM";
        case SEVERITY_HIGH:     return "HIGH";
        case SEVERITY_CRITICAL: return "CRITICAL";
        default:                return "NORMAL";
    }
}

bool list_init(IncidentList *list) {
    if (!list) return false;
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
    list->next_incident_id = 1;
    if (pthread_mutex_init(&list->lock, NULL) != 0) {
        return false;
    }
    return true;
}

void list_free(IncidentList *list) {
    if (!list) return;
    pthread_mutex_lock(&list->lock);
    IncidentNode *curr = list->head;
    while (curr) {
        IncidentNode *next = curr->next;
        free(curr);
        curr = next;
    }
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
    pthread_mutex_unlock(&list->lock);
    pthread_mutex_destroy(&list->lock);
}

int list_insert_incident(IncidentList *list, IncidentRecord record) {
    if (!list) return -1;

    IncidentNode *node = (IncidentNode *)malloc(sizeof(IncidentNode));
    if (!node) return -1;

    pthread_mutex_lock(&list->lock);
    record.incident_id = list->next_incident_id++;
    node->data = record;
    node->next = NULL;
    node->prev = list->tail;

    if (list->tail) {
        list->tail->next = node;
        list->tail = node;
    } else {
        list->head = node;
        list->tail = node;
    }
    list->size++;
    int assigned_id = record.incident_id;
    pthread_mutex_unlock(&list->lock);

    return assigned_id;
}

bool list_get_incident(IncidentList *list, int id, IncidentRecord *out_record) {
    if (!list || !out_record) return false;

    pthread_mutex_lock(&list->lock);
    IncidentNode *curr = list->head;
    while (curr) {
        if (curr->data.incident_id == id) {
            *out_record = curr->data;
            pthread_mutex_unlock(&list->lock);
            return true;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&list->lock);
    return false;
}

int list_size(IncidentList *list) {
    if (!list) return 0;
    pthread_mutex_lock(&list->lock);
    int s = list->size;
    pthread_mutex_unlock(&list->lock);
    return s;
}

void list_clear(IncidentList *list) {
    if (!list) return;
    pthread_mutex_lock(&list->lock);
    IncidentNode *curr = list->head;
    while (curr) {
        IncidentNode *next = curr->next;
        free(curr);
        curr = next;
    }
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
    pthread_mutex_unlock(&list->lock);
}

int list_to_summary_json(IncidentList *list, char *buffer, size_t max_size) {
    if (!list || !buffer || max_size < 3) {
        if (buffer && max_size >= 3) snprintf(buffer, max_size, "[]");
        return 0;
    }

    pthread_mutex_lock(&list->lock);
    size_t offset = 0;
    offset += snprintf(buffer + offset, max_size - offset, "[");

    IncidentNode *curr = list->head;
    bool first = true;
    while (curr && offset < max_size - 10) {
        if (!first) {
            offset += snprintf(buffer + offset, max_size - offset, ",");
        }
        first = false;

        IncidentRecord *r = &curr->data;
        offset += snprintf(buffer + offset, max_size - offset,
            "{\"id\":%d,\"timestamp\":%llu,\"type\":\"%s\",\"severity\":\"%s\","
            "\"peak_g\":%.2f,\"impact_speed\":%.1f,\"roll_angle\":%.1f,"
            "\"latitude\":%.6f,\"longitude\":%.6f,\"description\":\"%s\","
            "\"hospital\":\"%s\",\"hospital_dist\":%.2f,\"hospital_phone\":\"%s\",\"packet_count\":%d}",
            r->incident_id,
            (unsigned long long)r->timestamp_ms,
            anomaly_type_to_string(r->type),
            severity_to_string(r->severity),
            r->peak_g,
            r->impact_speed_kmh,
            r->roll_angle,
            r->latitude,
            r->longitude,
            r->description,
            r->nearest_station.name,
            r->nearest_station.distance_km,
            r->nearest_station.phone,
            r->packet_count);

        curr = curr->next;
    }

    offset += snprintf(buffer + offset, max_size - offset, "]");
    pthread_mutex_unlock(&list->lock);
    return (int)offset;
}

int list_incident_to_json(IncidentList *list, int id, char *buffer, size_t max_size) {
    if (!list || !buffer || max_size < 100) return 0;

    IncidentRecord record;
    if (!list_get_incident(list, id, &record)) {
        return snprintf(buffer, max_size, "{\"error\":\"Incident not found\"}");
    }

    size_t offset = 0;
    offset += snprintf(buffer + offset, max_size - offset,
        "{\"id\":%d,\"timestamp\":%llu,\"type\":\"%s\",\"severity\":\"%s\","
        "\"peak_g\":%.2f,\"impact_speed\":%.1f,\"roll_angle\":%.1f,"
        "\"latitude\":%.6f,\"longitude\":%.6f,\"description\":\"%s\","
        "\"hospital\":\"%s\",\"hospital_dist\":%.2f,\"hospital_phone\":\"%s\",\"trigger_index\":%d,\"packets\":[",
        record.incident_id,
        (unsigned long long)record.timestamp_ms,
        anomaly_type_to_string(record.type),
        severity_to_string(record.severity),
        record.peak_g,
        record.impact_speed_kmh,
        record.roll_angle,
        record.latitude,
        record.longitude,
        record.description,
        record.nearest_station.name,
        record.nearest_station.distance_km,
        record.nearest_station.phone,
        record.trigger_index);

    for (int i = 0; i < record.packet_count && offset < max_size - 120; i++) {
        TelemetryPacket *p = &record.packets[i];
        if (i > 0) {
            offset += snprintf(buffer + offset, max_size - offset, ",");
        }
        offset += snprintf(buffer + offset, max_size - offset,
            "{\"t\":%llu,\"speed\":%.1f,\"rpm\":%.0f,\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,\"g\":%.2f,"
            "\"pitch\":%.1f,\"roll\":%.1f,\"yaw\":%.1f,\"brake\":%.1f,\"temp\":%.1f,\"score\":%.2f}",
            (unsigned long long)p->timestamp_ms,
            p->speed_kmh, p->rpm,
            p->accel_x, p->accel_y, p->accel_z, p->total_g,
            p->gyro_pitch, p->gyro_roll, p->gyro_yaw,
            p->brake_pressure, p->engine_temp_c, p->anomaly_score);
    }

    offset += snprintf(buffer + offset, max_size - offset, "]}");
    return (int)offset;
}

/* ===========================================================================
 * Black-box persistence
 * File layout:  [BlackBoxHeader][IncidentRecord][IncidentRecord]...
 * A torn (half-written) last record is simply ignored on load, because
 * fread() will not return a complete record for it.
 * ========================================================================= */
#include <unistd.h>

#define BLACKBOX_MAGIC   0x45524154u /* "TARE" */
#define BLACKBOX_VERSION 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t record_size;
    uint32_t reserved;
} BlackBoxHeader;

bool list_append_to_file(const IncidentRecord *record, const char *path) {
    if (!record || !path) return false;

    FILE *f = fopen(path, "ab");
    if (!f) return false;

    /* Write header if the file is new/empty */
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) {
        BlackBoxHeader h = { BLACKBOX_MAGIC, BLACKBOX_VERSION, (uint32_t)sizeof(IncidentRecord), 0 };
        if (fwrite(&h, sizeof(h), 1, f) != 1) { fclose(f); return false; }
    }

    bool ok = (fwrite(record, sizeof(IncidentRecord), 1, f) == 1);
    fflush(f);
    fsync(fileno(f));   /* Force data onto the disk: survives power loss */
    fclose(f);
    return ok;
}

int list_load_from_file(IncidentList *list, const char *path) {
    if (!list || !path) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;   /* No file yet: nothing to restore */

    BlackBoxHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1 ||
        h.magic != BLACKBOX_MAGIC ||
        h.version != BLACKBOX_VERSION ||
        h.record_size != sizeof(IncidentRecord)) {
        fclose(f);
        return -1;      /* Incompatible / corrupt file */
    }

    IncidentRecord *rec = (IncidentRecord *)malloc(sizeof(IncidentRecord));
    if (!rec) { fclose(f); return -1; }

    int loaded = 0;
    while (fread(rec, sizeof(IncidentRecord), 1, f) == 1) {
        IncidentNode *node = (IncidentNode *)malloc(sizeof(IncidentNode));
        if (!node) break;
        node->data = *rec;
        node->next = NULL;

        pthread_mutex_lock(&list->lock);
        node->prev = list->tail;
        if (list->tail) list->tail->next = node; else list->head = node;
        list->tail = node;
        list->size++;
        if (rec->incident_id >= list->next_incident_id) {
            list->next_incident_id = rec->incident_id + 1;  /* keep IDs unique after restart */
        }
        pthread_mutex_unlock(&list->lock);
        loaded++;
    }

    free(rec);
    fclose(f);
    return loaded;
}

bool list_clear_file(const char *path) {
    if (!path) return false;
    return remove(path) == 0;
}
