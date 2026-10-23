#include "circular_queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool cq_init(CircularQueue *q, int capacity) {
    if (!q || capacity <= 0) return false;
    
    q->buffer = (TelemetryPacket *)malloc(sizeof(TelemetryPacket) * capacity);
    if (!q->buffer) return false;

    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->capacity = capacity;
    q->is_frozen = false;
    
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q->buffer);
        q->buffer = NULL;
        return false;
    }
    return true;
}

void cq_free(CircularQueue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    if (q->buffer) {
        free(q->buffer);
        q->buffer = NULL;
    }
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->capacity = 0;
    q->is_frozen = false;
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
}

bool cq_enqueue(CircularQueue *q, const TelemetryPacket *packet) {
    if (!q || !packet) return false;

    pthread_mutex_lock(&q->lock);
    if (q->is_frozen) {
        pthread_mutex_unlock(&q->lock);
        return false; /* Queue is frozen, cannot overwrite pre-crash data */
    }

    q->buffer[q->head] = *packet;

    if (q->count == q->capacity) {
        /* Buffer is full: overwrite oldest element, advance tail */
        q->tail = (q->tail + 1) % q->capacity;
    } else {
        q->count++;
    }

    q->head = (q->head + 1) % q->capacity;

    pthread_mutex_unlock(&q->lock);
    return true;
}

bool cq_dequeue(CircularQueue *q, TelemetryPacket *out_packet) {
    if (!q || !out_packet) return false;

    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    *out_packet = q->buffer[q->tail];
    q->tail = (q->tail + 1) % q->capacity;
    q->count--;

    pthread_mutex_unlock(&q->lock);
    return true;
}

bool cq_peek_latest(CircularQueue *q, TelemetryPacket *out_packet) {
    if (!q || !out_packet) return false;

    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    int latest_idx = (q->head - 1 + q->capacity) % q->capacity;
    *out_packet = q->buffer[latest_idx];

    pthread_mutex_unlock(&q->lock);
    return true;
}

int cq_get_all_ordered(CircularQueue *q, TelemetryPacket *out_array, int max_count) {
    if (!q || !out_array || max_count <= 0) return 0;

    pthread_mutex_lock(&q->lock);
    int to_copy = (q->count < max_count) ? q->count : max_count;
    for (int i = 0; i < to_copy; i++) {
        int idx = (q->tail + i) % q->capacity;
        out_array[i] = q->buffer[idx];
    }
    pthread_mutex_unlock(&q->lock);
    return to_copy;
}

void cq_freeze(CircularQueue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    q->is_frozen = true;
    pthread_mutex_unlock(&q->lock);
}

void cq_unfreeze(CircularQueue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    q->is_frozen = false;
    pthread_mutex_unlock(&q->lock);
}

bool cq_is_frozen(CircularQueue *q) {
    if (!q) return false;
    pthread_mutex_lock(&q->lock);
    bool frozen = q->is_frozen;
    pthread_mutex_unlock(&q->lock);
    return frozen;
}

void cq_clear(CircularQueue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->is_frozen = false;
    pthread_mutex_unlock(&q->lock);
}

void cq_get_stats(CircularQueue *q, int *head, int *tail, int *count, int *capacity, bool *is_frozen) {
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    if (head) *head = q->head;
    if (tail) *tail = q->tail;
    if (count) *count = q->count;
    if (capacity) *capacity = q->capacity;
    if (is_frozen) *is_frozen = q->is_frozen;
    pthread_mutex_unlock(&q->lock);
}
