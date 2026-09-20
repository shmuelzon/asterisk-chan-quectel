/*
 * audio.c - ALSA audio endpoint for the EC25-E UAC interface
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "audio.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "ringbuf.h"

/* How much audio each direction may buffer before old data is discarded. */
#define RX_BUFFER_MS 200
#define TX_BUFFER_MS 200

struct quectel_audio {
    char alsadev[64];
    unsigned int rate;
    unsigned int frame_samples;
    size_t frame_bytes;

    snd_pcm_t* cap;
    snd_pcm_t* ply;

    pthread_t thread;
    int thread_started;
    volatile int running;

    int evfd;

    struct ringbuf rx; /* modem -> Asterisk */
    struct ringbuf tx; /* Asterisk -> modem */

    quectel_audio_log_fn log;

    struct quectel_audio_stats stats;
    pthread_mutex_t stats_lock;
};

static void au_log(const struct quectel_audio* au, int level, const char* fmt, ...)
{
    if (!au->log) {
        return;
    }

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    au->log(level, buf);
}

/*
 * Configure one PCM.
 *
 * Capture is opened BLOCKING: the read then returns exactly when a period is
 * ready, which is what paces the audio thread. Playback is opened NON-BLOCKING
 * so a full playback buffer can never stall the capture side.
 */
static int open_pcm(struct quectel_audio* au, snd_pcm_stream_t dir, snd_pcm_t** out)
{
    const char* const what = (dir == SND_PCM_STREAM_CAPTURE) ? "capture" : "playback";
    snd_pcm_t* pcm         = NULL;

    int res = snd_pcm_open(&pcm, au->alsadev, dir, (dir == SND_PCM_STREAM_CAPTURE) ? 0 : SND_PCM_NONBLOCK);
    if (res < 0) {
        au_log(au, 3, "cannot open %s on %s: %s", what, au->alsadev, snd_strerror(res));
        return -1;
    }

    snd_pcm_hw_params_t* hw = alloca(snd_pcm_hw_params_sizeof());
    memset(hw, 0, snd_pcm_hw_params_sizeof());

    if ((res = snd_pcm_hw_params_any(pcm, hw)) < 0) {
        au_log(au, 3, "%s: no configurations available: %s", what, snd_strerror(res));
        goto fail;
    }
    if ((res = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        au_log(au, 3, "%s: cannot set access: %s", what, snd_strerror(res));
        goto fail;
    }
    if ((res = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0) {
        au_log(au, 3, "%s: cannot set S16_LE: %s", what, snd_strerror(res));
        goto fail;
    }

    unsigned int channels = 1;
    if ((res = snd_pcm_hw_params_set_channels_near(pcm, hw, &channels)) < 0) {
        au_log(au, 3, "%s: cannot set channels: %s", what, snd_strerror(res));
        goto fail;
    }
    if (channels != 1) {
        au_log(au, 3, "%s: device insists on %u channels, only mono is supported", what, channels);
        goto fail;
    }

    unsigned int rate = au->rate;
    if ((res = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0) {
        au_log(au, 3, "%s: cannot set rate: %s", what, snd_strerror(res));
        goto fail;
    }
    if (rate != au->rate) {
        au_log(au, 3, "%s: device gave %u Hz, wanted %u Hz", what, rate, au->rate);
        goto fail;
    }

    /*
     * One period per Asterisk frame, with a few periods of slack in the buffer.
     * set_*_near is used throughout: if the device refuses these exact sizes we
     * take what it offers rather than failing to open.
     */
    snd_pcm_uframes_t period = au->frame_samples;
    if ((res = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0) {
        au_log(au, 4, "%s: cannot set period size: %s", what, snd_strerror(res));
    }

    snd_pcm_uframes_t buffer = au->frame_samples * 8;
    if ((res = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0) {
        au_log(au, 4, "%s: cannot set buffer size: %s", what, snd_strerror(res));
    }

    if ((res = snd_pcm_hw_params(pcm, hw)) < 0) {
        au_log(au, 3, "%s: cannot apply hw params: %s", what, snd_strerror(res));
        goto fail;
    }

    snd_pcm_uframes_t real_period = au->frame_samples;
    snd_pcm_uframes_t real_buffer = au->frame_samples * 8;
    snd_pcm_get_params(pcm, &real_buffer, &real_period);

    {
        snd_pcm_sw_params_t* sw = alloca(snd_pcm_sw_params_sizeof());
        memset(sw, 0, snd_pcm_sw_params_sizeof());
        if ((res = snd_pcm_sw_params_current(pcm, sw)) == 0) {
            /*
             * Playback: do not let the device start until a few periods are
             * queued, otherwise it starts on the first write and immediately
             * underruns waiting for the next one. Capture: start as soon as
             * there is anything at all.
             */
            const snd_pcm_uframes_t start = (dir == SND_PCM_STREAM_PLAYBACK) ? real_period * 3 : 1;
            snd_pcm_sw_params_set_start_threshold(pcm, sw, start);
            /* Never let a transient stall stop the stream outright. */
            snd_pcm_sw_params_set_stop_threshold(pcm, sw, real_buffer * 2);
            snd_pcm_sw_params_set_avail_min(pcm, sw, real_period);
            if ((res = snd_pcm_sw_params(pcm, sw)) < 0) {
                au_log(au, 4, "%s: cannot apply sw params: %s", what, snd_strerror(res));
            }
        }
    }

    snd_pcm_uframes_t got_period = 0, got_buffer = 0;
    snd_pcm_get_params(pcm, &got_buffer, &got_period);
    au_log(au, 6, "%s ready on %s: %u Hz mono, period %lu, buffer %lu", what, au->alsadev, rate, (unsigned long)got_period, (unsigned long)got_buffer);

    *out = pcm;
    return 0;

fail:
    snd_pcm_close(pcm);
    return -1;
}

static void stats_bump(struct quectel_audio* au, unsigned long* field, unsigned long by)
{
    pthread_mutex_lock(&au->stats_lock);
    *field += by;
    pthread_mutex_unlock(&au->stats_lock);
}

/* Recover a stream after an overrun/underrun. */
static int recover(struct quectel_audio* au, snd_pcm_t* pcm, int err, const char* what)
{
    if (err == -EPIPE || err == -ESTRPIPE) {
        const int res = snd_pcm_recover(pcm, err, 1 /* silent */);
        if (res < 0) {
            au_log(au, 4, "%s: recovery failed: %s", what, snd_strerror(res));
            return -1;
        }
        return 0;
    }
    return -1;
}

static void* audio_thread(void* arg)
{
    struct quectel_audio* const au = arg;
    const size_t frame_bytes    = au->frame_bytes;

    short* const inbuf  = malloc(frame_bytes);
    short* const outbuf = malloc(frame_bytes);
    if (!inbuf || !outbuf) {
        au_log(au, 3, "audio thread: out of memory");
        free(inbuf);
        free(outbuf);
        return NULL;
    }

    /*
     * A prepared capture stream produces nothing until it is started. Start it
     * explicitly rather than relying on the first read to do it implicitly.
     */
    int res = snd_pcm_start(au->cap);
    if (res < 0) {
        au_log(au, 4, "capture: start failed: %s", snd_strerror(res));
    }

    /* Prime playback so it has a cushion before the device starts consuming. */
    memset(outbuf, 0, frame_bytes);
    for (int i = 0; i < 4; i++) {
        snd_pcm_writei(au->ply, outbuf, au->frame_samples);
    }

    while (au->running) {
        /* --- capture: blocking, this is what paces the loop --- */
        res = snd_pcm_readi(au->cap, inbuf, au->frame_samples);
        if (res < 0) {
            if (res == -EAGAIN) {
                continue;
            }
            if (res == -EPIPE) {
                stats_bump(au, &au->stats.capture_xruns, 1);
            }
            if (recover(au, au->cap, res, "capture") < 0) {
                stats_bump(au, &au->stats.capture_errors, 1);
                if (res == -ENODEV || res == -ENOTTY) {
                    au_log(au, 3, "capture: device gone, stopping audio thread");
                    break;
                }
                /* Avoid a hot spin if the device is wedged. */
                usleep(5000);
            }
            snd_pcm_start(au->cap);
            continue;
        }

        if (res > 0) {
            const size_t got     = (size_t)res * sizeof(short);
            const size_t dropped = ringbuf_write(&au->rx, inbuf, got);
            if (dropped) {
                stats_bump(au, &au->stats.rx_dropped, dropped);
            }
            stats_bump(au, &au->stats.frames_captured, 1);

            /* Wake the PBX thread - one token per captured frame. */
            const uint64_t one = 1;
            if (write(au->evfd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
                /* nothing useful to do; the channel also polls on a timer */
            }
        }

        /* --- playback: send whatever the PBX queued, silence otherwise --- */
        if (!ringbuf_read(&au->tx, outbuf, frame_bytes)) {
            memset(outbuf, 0, frame_bytes);
            stats_bump(au, &au->stats.tx_silence, 1);
        }

        res = snd_pcm_writei(au->ply, outbuf, au->frame_samples);
        if (res < 0) {
            if (res == -EPIPE) {
                stats_bump(au, &au->stats.playback_xruns, 1);
            }
            if (res != -EAGAIN && recover(au, au->ply, res, "playback") < 0) {
                stats_bump(au, &au->stats.playback_errors, 1);
            }
        } else {
            stats_bump(au, &au->stats.frames_played, 1);
        }
    }

    free(inbuf);
    free(outbuf);
    return NULL;
}

struct quectel_audio* quectel_audio_create(const char* alsadev, unsigned int rate, unsigned int frame_samples, quectel_audio_log_fn log)
{
    if (!alsadev || !*alsadev || !rate || !frame_samples) {
        return NULL;
    }

    struct quectel_audio* const au = calloc(1, sizeof(*au));
    if (!au) {
        return NULL;
    }

    snprintf(au->alsadev, sizeof(au->alsadev), "%s", alsadev);
    au->rate          = rate;
    au->frame_samples = frame_samples;
    au->frame_bytes   = (size_t)frame_samples * sizeof(short);
    au->log           = log;
    au->evfd          = -1;

    pthread_mutex_init(&au->stats_lock, NULL);

    const size_t bytes_per_ms = (size_t)(rate / 1000) * sizeof(short);
    if (ringbuf_init(&au->rx, bytes_per_ms * RX_BUFFER_MS) < 0 || ringbuf_init(&au->tx, bytes_per_ms * TX_BUFFER_MS) < 0) {
        quectel_audio_destroy(au);
        return NULL;
    }

    return au;
}

int quectel_audio_start(struct quectel_audio* au)
{
    if (!au) {
        return -1;
    }
    if (au->running) {
        return 0; /* already streaming */
    }

    au->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (au->evfd < 0) {
        au_log(au, 3, "cannot create eventfd: %s", strerror(errno));
        return -1;
    }

    if (open_pcm(au, SND_PCM_STREAM_CAPTURE, &au->cap) < 0) {
        goto fail;
    }
    if (open_pcm(au, SND_PCM_STREAM_PLAYBACK, &au->ply) < 0) {
        goto fail;
    }

    ringbuf_reset(&au->rx);
    ringbuf_reset(&au->tx);

    au->running = 1;
    if (pthread_create(&au->thread, NULL, audio_thread, au) != 0) {
        au_log(au, 3, "cannot start audio thread: %s", strerror(errno));
        au->running = 0;
        goto fail;
    }
    au->thread_started = 1;

    return 0;

fail:
    quectel_audio_stop(au);
    return -1;
}

void quectel_audio_stop(struct quectel_audio* au)
{
    if (!au) {
        return;
    }

    au->running = 0;

    if (au->thread_started) {
        /*
         * Just join. A blocking snd_pcm_readi() returns as soon as one period
         * is ready - about 20 ms - so the thread notices running == 0 promptly
         * on its own. Do NOT call snd_pcm_drop() here to hurry it along: the
         * handle is owned by that thread and snd_pcm_t is not thread-safe, so
         * touching it concurrently corrupts ALSA's state (and crashed Asterisk
         * on module unload).
         */
        pthread_join(au->thread, NULL);
        au->thread_started = 0;
    }

    if (au->cap) {
        snd_pcm_close(au->cap);
        au->cap = NULL;
    }
    if (au->ply) {
        snd_pcm_close(au->ply);
        au->ply = NULL;
    }
    if (au->evfd >= 0) {
        close(au->evfd);
        au->evfd = -1;
    }
}

void quectel_audio_destroy(struct quectel_audio* au)
{
    if (!au) {
        return;
    }
    quectel_audio_stop(au);
    ringbuf_destroy(&au->rx);
    ringbuf_destroy(&au->tx);
    pthread_mutex_destroy(&au->stats_lock);
    free(au);
}

int quectel_audio_fd(const struct quectel_audio* au) { return au ? au->evfd : -1; }

size_t quectel_audio_frame_bytes(const struct quectel_audio* au) { return au ? au->frame_bytes : 0; }

int quectel_audio_read(struct quectel_audio* au, void* buf)
{
    if (!au) {
        return 0;
    }

    /* Consume one wakeup token; harmless if the counter is already drained. */
    uint64_t val;
    if (read(au->evfd, &val, sizeof(val)) < 0) {
        /* EAGAIN just means no token pending - the ring buffer is authoritative. */
    }

    if (!ringbuf_read(&au->rx, buf, au->frame_bytes)) {
        stats_bump(au, &au->stats.rx_underruns, 1);
        return 0;
    }

    return 1;
}

int quectel_audio_write(struct quectel_audio* au, const void* buf, size_t len)
{
    if (!au || !buf || !len) {
        return -1;
    }
    ringbuf_write(&au->tx, buf, len);
    return 0;
}

void quectel_audio_flush(struct quectel_audio* au)
{
    if (!au) {
        return;
    }
    ringbuf_reset(&au->rx);
    ringbuf_reset(&au->tx);
}

void quectel_audio_get_stats(const struct quectel_audio* au, struct quectel_audio_stats* out)
{
    if (!au || !out) {
        return;
    }
    struct quectel_audio* const m = (struct quectel_audio*)au;
    pthread_mutex_lock(&m->stats_lock);
    *out = au->stats;
    pthread_mutex_unlock(&m->stats_lock);
    out->rx_dropped += au->rx.overruns;
}
