#ifndef LINKED_LIST_H
#define LINKED_LIST_H

#include "telemetry.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct IncidentNode {
    IncidentRecord data;
    struct IncidentNode *prev;
    struct IncidentNode *next;
} IncidentNode;

typedef struct {
    IncidentNode *head;
    IncidentNode *tail;
    int size;
    int next_incident_id;
    pthread_mutex_t lock;
} IncidentList;

/* Initialize the incident linked list */
bool list_init(IncidentList *list);

/* Free all nodes in the linked list */
void list_free(IncidentList *list);

/* Append an incident to the tail of the list (O(1)) */
int list_insert_incident(IncidentList *list, IncidentRecord record);

/* Retrieve full incident record by ID (thread-safe copy) */
bool list_get_incident(IncidentList *list, int id, IncidentRecord *out_record);

/* Serialize incident summaries to JSON array (for dashboard table/history) */
int list_to_summary_json(IncidentList *list, char *buffer, size_t max_size);

/* Serialize a single incident including its full pre/post crash telemetry packet window (for reconstruction) */
int list_incident_to_json(IncidentList *list, int id, char *buffer, size_t max_size);

/* Get number of recorded incidents */
int list_size(IncidentList *list);

/* Clear all incidents */
void list_clear(IncidentList *list);

/* ---- Black-box persistence (Data Resilience) ----
 * Every committed incident is appended to a binary file and fsync()'d,
 * so it survives a crash / power loss. On start-up the file is read back
 * and the linked list is rebuilt node by node. */
bool list_append_to_file(const IncidentRecord *record, const char *path);
int  list_load_from_file(IncidentList *list, const char *path); /* returns nodes loaded, -1 on bad file */
bool list_clear_file(const char *path);

#endif /* LINKED_LIST_H */
