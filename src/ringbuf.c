/*
 * ringbuf.c
 */

#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

int ringbuf_init(struct ringbuf* rb, size_t size)
{
    memset(rb, 0, sizeof(*rb));
    rb->data = malloc(size);
    if (!rb->data) {
        return -1;
    }
    rb->size = size;
    pthread_mutex_init(&rb->lock, NULL);
    return 0;
}

void ringbuf_destroy(struct ringbuf* rb)
{
    if (!rb->data) {
        return;
    }
    pthread_mutex_destroy(&rb->lock);
    free(rb->data);
    rb->data = NULL;
    rb->size = rb->used = rb->head = rb->tail = 0;
}

void ringbuf_reset(struct ringbuf* rb)
{
    pthread_mutex_lock(&rb->lock);
    rb->head = rb->tail = rb->used = 0;
    pthread_mutex_unlock(&rb->lock);
}

size_t ringbuf_used(struct ringbuf* rb)
{
    pthread_mutex_lock(&rb->lock);
    const size_t used = rb->used;
    pthread_mutex_unlock(&rb->lock);
    return used;
}

static void copy_in(struct ringbuf* rb, const unsigned char* src, size_t len)
{
    const size_t first = (rb->size - rb->head < len) ? rb->size - rb->head : len;
    memcpy(rb->data + rb->head, src, first);
    if (len > first) {
        memcpy(rb->data, src + first, len - first);
    }
    rb->head  = (rb->head + len) % rb->size;
    rb->used += len;
}

static void copy_out(struct ringbuf* rb, unsigned char* dst, size_t len)
{
    const size_t first = (rb->size - rb->tail < len) ? rb->size - rb->tail : len;
    memcpy(dst, rb->data + rb->tail, first);
    if (len > first) {
        memcpy(dst + first, rb->data, len - first);
    }
    rb->tail  = (rb->tail + len) % rb->size;
    rb->used -= len;
}

size_t ringbuf_write(struct ringbuf* rb, const void* src, size_t len)
{
    size_t dropped = 0;

    if (len > rb->size) {
        /* Caller handed us more than the buffer holds - keep only the tail of it. */
        dropped = len - rb->size;
        src     = (const unsigned char*)src + dropped;
        len     = rb->size;
    }

    pthread_mutex_lock(&rb->lock);

    const size_t space = rb->size - rb->used;
    if (len > space) {
        /* Discard oldest audio to make room for the newest. */
        const size_t discard  = len - space;
        rb->tail              = (rb->tail + discard) % rb->size;
        rb->used             -= discard;
        dropped              += discard;
        rb->overruns++;
    }

    copy_in(rb, src, len);
    pthread_mutex_unlock(&rb->lock);

    return dropped;
}

int ringbuf_read(struct ringbuf* rb, void* dst, size_t len)
{
    pthread_mutex_lock(&rb->lock);

    if (rb->used < len) {
        rb->underruns++;
        pthread_mutex_unlock(&rb->lock);
        return 0;
    }

    copy_out(rb, dst, len);
    pthread_mutex_unlock(&rb->lock);

    return 1;
}
