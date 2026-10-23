#ifndef CIRCULAR_QUEUE_H
#define CIRCULAR_QUEUE_H

#include "telemetry.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    TelemetryPacket *buffer;
    int head;       /* Write index for next packet */
    int tail;       /* Read index for oldest packet */
    int count;      /* Current number of items stored */
    int capacity;   /* Max buffer capacity */
    bool is_frozen; /* When true, overwrites are prevented (crash black-box freeze) */
    pthread_mutex_t lock;
} CircularQueue;

/* Initialize circular queue with given capacity */
bool cq_init(CircularQueue *q, int capacity);

/* Destroy circular queue and release memory/mutex */
void cq_free(CircularQueue *q);

/* Enqueue a packet. When full, silently overwrites oldest element (updating tail) unless frozen.
   Returns true on success, false if frozen. */
bool cq_enqueue(CircularQueue *q, const TelemetryPacket *packet);

/* Dequeue the oldest packet */
bool cq_dequeue(CircularQueue *q, TelemetryPacket *out_packet);

/* Peek at the latest (most recently inserted) packet */
bool cq_peek_latest(CircularQueue *q, TelemetryPacket *out_packet);

/* Extract all packets in chronological order (oldest to newest) without altering queue state */
int cq_get_all_ordered(CircularQueue *q, TelemetryPacket *out_array, int max_count);

/* Freeze the circular buffer on critical crash to prevent overwriting */
void cq_freeze(CircularQueue *q);

/* Unfreeze the circular buffer to resume recording */
void cq_unfreeze(CircularQueue *q);

/* Check freeze status */
bool cq_is_frozen(CircularQueue *q);

/* Clear buffer contents */
void cq_clear(CircularQueue *q);

/* Get internal diagnostics for dashboard visualizer */
void cq_get_stats(CircularQueue *q, int *head, int *tail, int *count, int *capacity, bool *is_frozen);

#endif /* CIRCULAR_QUEUE_H */
