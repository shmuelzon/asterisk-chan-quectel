/*
 * ringbuf.h - byte ring buffer shared between the audio thread and the PBX thread
 */

#ifndef CHAN_QUECTEL_RINGBUF_H
#define CHAN_QUECTEL_RINGBUF_H

#include <pthread.h>
#include <stddef.h>

struct ringbuf {
    unsigned char* data;
    size_t size; /* capacity in bytes */
    size_t head; /* write position */
    size_t tail; /* read position  */
    size_t used; /* bytes currently stored */
    pthread_mutex_t lock;
    /* statistics */
    unsigned long overruns;  /* writes that had to drop data */
    unsigned long underruns; /* reads that found too little data */
};

int ringbuf_init(struct ringbuf* rb, size_t size);
void ringbuf_destroy(struct ringbuf* rb);

/* Drop everything currently buffered. */
void ringbuf_reset(struct ringbuf* rb);

size_t ringbuf_used(struct ringbuf* rb);

/*
 * Append len bytes. When the buffer is full the OLDEST data is discarded so the
 * newest audio always wins - for live audio a late frame is worthless.
 * Returns the number of bytes dropped to make room.
 */
size_t ringbuf_write(struct ringbuf* rb, const void* src, size_t len);

/*
 * Remove exactly len bytes. Returns 1 on success, 0 if fewer than len bytes are
 * available (in which case nothing is consumed and underruns is incremented).
 */
int ringbuf_read(struct ringbuf* rb, void* dst, size_t len);

#endif /* CHAN_QUECTEL_RINGBUF_H */
