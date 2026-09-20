/*
 * audio.h - ALSA audio endpoint for the EC25-E USB Audio Class interface
 *
 * The EC25-E exposes a plain UAC device (card "Android"): 8 kHz, S16_LE, mono,
 * one stream in each direction.
 *
 * Design notes - these are the reason this file exists at all:
 *
 *   - A dedicated thread owns both PCM handles and does all ALSA I/O. The
 *     capture read is BLOCKING, which paces the loop at exactly the device
 *     clock with no timer, no sleep and no drift.
 *
 *   - The PBX thread never touches ALSA. It exchanges audio through ring
 *     buffers, so a slow or bursty PBX cannot stall the capture stream.
 *
 *   - Readability is signalled to Asterisk on an eventfd, NOT on the ALSA poll
 *     descriptor. The descriptor returned by snd_pcm_poll_descriptors() did not
 *     work as a wakeup source on this device, which is what made the previous
 *     driver deliver roughly a tenth of the frames it should.
 *
 * This header deliberately depends only on ALSA and pthreads (logging goes
 * through a callback) so the endpoint can be exercised by a standalone test
 * harness without linking against Asterisk.
 */

#ifndef CHAN_QUECTEL_AUDIO_H
#define CHAN_QUECTEL_AUDIO_H

#include <stddef.h>

struct quectel_audio;

/* level mirrors syslog: 3 = error, 4 = warning, 6 = info, 7 = debug */
typedef void (*quectel_audio_log_fn)(int level, const char* msg);

struct quectel_audio_stats {
    unsigned long frames_captured;
    unsigned long frames_played;
    unsigned long capture_xruns;
    unsigned long playback_xruns;
    unsigned long capture_errors;
    unsigned long playback_errors;
    unsigned long rx_dropped;   /* bytes discarded because the PBX did not keep up */
    unsigned long tx_silence;   /* frames of silence sent because the PBX had nothing */
    unsigned long rx_underruns; /* channel_read found no complete frame */
};

/*
 * alsadev        - e.g. "hw:Android"
 * rate           - 8000
 * frame_samples  - samples per Asterisk frame (160 for 20 ms at 8 kHz)
 */
struct quectel_audio* quectel_audio_create(const char* alsadev, unsigned int rate, unsigned int frame_samples, quectel_audio_log_fn log);
void quectel_audio_destroy(struct quectel_audio* au);

/*
 * Opens the PCMs and starts the audio thread. Idempotent - calling it while
 * already streaming is a no-op.
 *
 * The stream is only run while a call is up. Leaving it running when idle keeps
 * the USB audio endpoint transferring in both directions for nothing, which
 * costs a few percent of a CPU core permanently and makes the ring buffers
 * churn with audio no one reads.
 */
int quectel_audio_start(struct quectel_audio* au);
void quectel_audio_stop(struct quectel_audio* au);

/* eventfd that becomes readable when at least one full frame can be read. */
int quectel_audio_fd(const struct quectel_audio* au);

/* Bytes in one frame (frame_samples * 2). */
size_t quectel_audio_frame_bytes(const struct quectel_audio* au);

/* Pop exactly one frame. Returns 1 on success, 0 when nothing is buffered. */
int quectel_audio_read(struct quectel_audio* au, void* buf);

/* Queue audio for playback. len must be a whole number of samples. */
int quectel_audio_write(struct quectel_audio* au, const void* buf, size_t len);

/* Drop everything buffered in both directions (use when a call starts). */
void quectel_audio_flush(struct quectel_audio* au);

void quectel_audio_get_stats(const struct quectel_audio* au, struct quectel_audio_stats* out);

#endif /* CHAN_QUECTEL_AUDIO_H */
