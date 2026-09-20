/*
 * audio_harness.c - exercise the audio endpoint without Asterisk.
 *
 *   audio_harness <alsadev> <seconds> [echo]
 *
 * Reads frames the way channel_read() will, at the 20 ms cadence Asterisk uses.
 * With "echo" it writes each captured frame straight back so the far end hears
 * itself - a genuine two-way audio test.
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../src/audio.h"

static void logcb(int level, const char* msg) { fprintf(stderr, "[%d] %s\n", level, msg); }

int main(int argc, char** argv)
{
    const char* dev = (argc > 1) ? argv[1] : "hw:Android";
    int secs        = (argc > 2) ? atoi(argv[2]) : 10;
    int do_echo     = (argc > 3 && !strcmp(argv[3], "echo"));

    struct quectel_audio* au = quectel_audio_create(dev, 8000, 160, logcb);
    if (!au || quectel_audio_start(au) < 0) { fprintf(stderr, "start failed\n"); return 1; }

    const size_t fb = quectel_audio_frame_bytes(au);
    short* buf = malloc(fb);
    FILE* out = fopen("/tmp/harness.raw", "wb");

    const int ticks = secs * 50;
    int got = 0, empty = 0;
    struct pollfd pfd = { .fd = quectel_audio_fd(au), .events = POLLIN };

    for (int i = 0; i < ticks; i++) {
        poll(&pfd, 1, 40);                 /* wait like Asterisk waits on the channel fd */
        if (quectel_audio_read(au, buf)) {
            got++;
            if (out) fwrite(buf, 1, fb, out);
            if (do_echo) quectel_audio_write(au, buf, fb);
        } else {
            empty++;
            if (out) { void* z = calloc(1, fb); fwrite(z, 1, fb, out); free(z); }
        }
    }

    struct quectel_audio_stats st;
    quectel_audio_get_stats(au, &st);
    printf("RESULT frames_read=%d/%d empty=%d | captured=%lu played=%lu "
           "cap_xrun=%lu ply_xrun=%lu tx_silence=%lu rx_dropped=%lu\n",
           got, ticks, empty, st.frames_captured, st.frames_played,
           st.capture_xruns, st.playback_xruns, st.tx_silence, st.rx_dropped);

    if (out) fclose(out);
    quectel_audio_destroy(au);
    free(buf);
    return 0;
}
