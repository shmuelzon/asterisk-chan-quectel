/* at_harness.c - exercise the AT transport without Asterisk */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/at.h"

static void logcb(int lvl, const char* m) { if (lvl <= 6) fprintf(stderr, "[%d] %s\n", lvl, m); }
static void urccb(void* u, const char* line) { (void)u; printf("URC: %s\n", line); }
static void failcb(void* u) { (void)u; printf("PORT FAILED\n"); }

int main(int argc, char** argv)
{
    const char* tty = (argc > 1) ? argv[1] : "/dev/ttyUSB2";
    struct quectel_at* at = quectel_at_open(tty, logcb, urccb, failcb, NULL);
    if (!at) { fprintf(stderr, "open failed\n"); return 1; }

    struct { const char* cmd; } probes[] = {
        {"AT"}, {"ATE0"}, {"AT+CGMM"}, {"AT+CGSN"}, {"AT+CSQ"},
        {"AT+QPCMV?"}, {"AT+QRXGAIN?"}, {"AT+CLVL?"}, {"AT+COPS?"}, {"AT+NONSENSE"},
    };
    char resp[512];
    for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        const int rc = quectel_at_command(at, probes[i].cmd, 5000, resp, sizeof(resp));
        printf("%-14s rc=%-2d  %s\n", probes[i].cmd, rc, resp[0] ? resp : "(no payload)");
    }

    printf("--- listening for URCs for 5s ---\n");
    sleep(5);
    quectel_at_close(at);
    printf("closed cleanly\n");
    return 0;
}
