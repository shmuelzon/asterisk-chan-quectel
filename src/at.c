/*
 * at.c - AT command transport for the EC25-E
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "at.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define AT_LINE_MAX 1024
#define AT_RESP_MAX 4096

struct quectel_at {
    int fd;
    int quitfd; /* eventfd used to wake the reader out of poll() */

    pthread_t reader;
    int reader_started;
    volatile int reader_running_cb; /* inside the URC callback right now */
    volatile int running;

    /* Serialises command submission - only one outstanding command at a time. */
    pthread_mutex_t cmd_lock;

    /* Guards the fields below plus the completion condvar. */
    pthread_mutex_t lock;
    pthread_cond_t cond;

    int pending;               /* a command is awaiting its final response */
    int result;                /* 0 = OK, -1 = ERROR, set when done */
    int done;                  /* final response seen */
    char resp[AT_RESP_MAX];    /* accumulated intermediate lines */
    size_t resp_used;

    quectel_at_log_fn log;
    quectel_at_urc_fn urc;
    quectel_at_fail_fn fail;
    void* userdata;
};

static void at_log(const struct quectel_at* at, int level, const char* fmt, ...)
{
    if (!at->log) {
        return;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char buf[560];
    snprintf(buf, sizeof(buf), "[tid %lu] %s", (unsigned long)pthread_self(), msg);
    at->log(level, buf);
}

static int configure_port(int fd)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) < 0) {
        return -1;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);

    tio.c_cflag     |= CLOCAL | CREAD;
    tio.c_cflag     &= ~CRTSCTS;
    tio.c_cc[VMIN]   = 0;
    tio.c_cc[VTIME]  = 0;

    return tcsetattr(fd, TCSANOW, &tio);
}

/* Is this line the terminator of a command response? */
static int is_final(const char* line, int* result)
{
    if (!strcmp(line, "OK")) {
        *result = 0;
        return 1;
    }
    if (!strcmp(line, "ERROR") || !strcmp(line, "NO CARRIER") || !strcmp(line, "NO ANSWER") || !strcmp(line, "NO DIALTONE") || !strcmp(line, "BUSY")) {
        *result = -1;
        return 1;
    }
    if (!strncmp(line, "+CME ERROR:", 11) || !strncmp(line, "+CMS ERROR:", 11)) {
        *result = -1;
        return 1;
    }
    return 0;
}

/*
 * URCs the modem can emit at any moment. Everything else that arrives while a
 * command is outstanding is treated as that command's intermediate response.
 */
static int is_urc(const char* line)
{
    static const char* const prefixes[] = {
        "RING", "+CLIP:", "+QIND:", "+CMTI:", "+CMT:", "+CDS:", "+CDSI:", "+CUSD:", "+CREG:", "+CEREG:", "+CGREG:", "NO CARRIER", "+QUSIM:", "POWERED DOWN", NULL,
    };

    for (int i = 0; prefixes[i]; i++) {
        if (!strncmp(line, prefixes[i], strlen(prefixes[i]))) {
            return 1;
        }
    }
    return 0;
}

static void handle_line(struct quectel_at* at, const char* line)
{
    if (!*line) {
        return;
    }

    at_log(at, 7, "< %s", line);

    int result = 0;
    const int final = is_final(line, &result);

    /*
     * "NO CARRIER" is both a call URC and a possible command terminator. Report
     * it as a URC as well so call teardown is noticed.
     */
    if (is_urc(line)) {
        if (at->urc) {
            at->reader_running_cb = 1;
            at->urc(at->userdata, line);
            at->reader_running_cb = 0;
        }
        if (!final) {
            return;
        }
    }

    pthread_mutex_lock(&at->lock);

    if (!at->pending) {
        pthread_mutex_unlock(&at->lock);
        return; /* nothing waiting - stray line */
    }

    if (final) {
        at->result = result;
        at->done   = 1;
        pthread_cond_signal(&at->cond);
    } else {
        const size_t len = strlen(line);
        if (at->resp_used + len + 2 < sizeof(at->resp)) {
            if (at->resp_used) {
                at->resp[at->resp_used++] = '\n';
            }
            memcpy(at->resp + at->resp_used, line, len);
            at->resp_used             += len;
            at->resp[at->resp_used]    = '\0';
        }
    }

    pthread_mutex_unlock(&at->lock);
}

static void* reader_thread(void* arg)
{
    struct quectel_at* const at = arg;
    char line[AT_LINE_MAX];
    size_t linelen = 0;

    struct pollfd pfd[2] = {
        {.fd = at->fd,     .events = POLLIN},
        {.fd = at->quitfd, .events = POLLIN},
    };

    while (at->running) {
        const int pres = poll(pfd, 2, 1000);
        if (pres < 0) {
            if (errno == EINTR) {
                continue;
            }
            at_log(at, 3, "poll failed: %s", strerror(errno));
            break;
        }
        if (pres == 0) {
            continue;
        }
        if (pfd[1].revents & POLLIN) {
            break; /* asked to stop */
        }
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            at_log(at, 3, "serial port hung up");
            break;
        }
        if (!(pfd[0].revents & POLLIN)) {
            continue;
        }

        char buf[512];
        const ssize_t n = read(at->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            at_log(at, 3, "read failed: %s", strerror(errno));
            break;
        }
        if (n == 0) {
            continue;
        }

        for (ssize_t i = 0; i < n; i++) {
            const char c = buf[i];
            if (c == '\r' || c == '\n') {
                if (linelen) {
                    line[linelen] = '\0';
                    handle_line(at, line);
                    linelen = 0;
                }
            } else if (linelen < sizeof(line) - 1) {
                line[linelen++] = c;
            }
            /* Overlong lines are truncated rather than split. */
        }
    }

    /* Unblock anyone waiting on a command. */
    pthread_mutex_lock(&at->lock);
    if (at->pending && !at->done) {
        at->result = -3;
        at->done   = 1;
        pthread_cond_signal(&at->cond);
    }
    pthread_mutex_unlock(&at->lock);

    const int was_running = at->running;
    at->running           = 0;

    if (was_running && at->fail) {
        at->fail(at->userdata);
    }

    return NULL;
}

struct quectel_at* quectel_at_open(const char* tty, quectel_at_log_fn log, quectel_at_urc_fn urc, quectel_at_fail_fn fail, void* userdata)
{
    if (!tty || !*tty) {
        return NULL;
    }

    struct quectel_at* const at = calloc(1, sizeof(*at));
    if (!at) {
        return NULL;
    }

    at->log      = log;
    at->urc      = urc;
    at->fail     = fail;
    at->userdata = userdata;
    at->quitfd   = -1;

    at->fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (at->fd < 0) {
        at_log(at, 3, "cannot open %s: %s", tty, strerror(errno));
        free(at);
        return NULL;
    }

    if (!isatty(at->fd)) {
        at_log(at, 3, "%s is not a tty", tty);
        close(at->fd);
        free(at);
        return NULL;
    }

    if (configure_port(at->fd) < 0) {
        at_log(at, 3, "cannot configure %s: %s", tty, strerror(errno));
        close(at->fd);
        free(at);
        return NULL;
    }

    /* Discard anything the modem left in the buffer from a previous owner. */
    tcflush(at->fd, TCIOFLUSH);

    at->quitfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (at->quitfd < 0) {
        close(at->fd);
        free(at);
        return NULL;
    }

    pthread_mutex_init(&at->cmd_lock, NULL);
    pthread_mutex_init(&at->lock, NULL);
    pthread_cond_init(&at->cond, NULL);

    at->running = 1;
    if (pthread_create(&at->reader, NULL, reader_thread, at) != 0) {
        at_log(at, 3, "cannot start reader thread");
        at->running = 0;
        quectel_at_close(at);
        return NULL;
    }
    at->reader_started = 1;

    return at;
}

void quectel_at_close(struct quectel_at* at)
{
    if (!at) {
        return;
    }

    at->running = 0;

    if (at->reader_started) {
        const uint64_t one = 1;
        if (at->quitfd >= 0) {
            (void)!write(at->quitfd, &one, sizeof(one));
        }
        pthread_join(at->reader, NULL);
        at->reader_started = 0;
    }

    if (at->fd >= 0) {
        close(at->fd);
        at->fd = -1;
    }
    if (at->quitfd >= 0) {
        close(at->quitfd);
        at->quitfd = -1;
    }

    pthread_cond_destroy(&at->cond);
    pthread_mutex_destroy(&at->lock);
    pthread_mutex_destroy(&at->cmd_lock);
    free(at);
}

static int write_all(int fd, const void* buf, size_t len)
{
    const unsigned char* p = buf;

    while (len) {
        const ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                if (poll(&pfd, 1, 1000) <= 0) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }

    return 0;
}

/*
 * Wait for the outstanding command's final response. Caller must already hold
 * cmd_lock and have set pending; the lock is NOT released here.
 */
static int wait_for_final(struct quectel_at* at, int timeout_ms, char* resp, size_t resp_len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    int rc = 0;
    pthread_mutex_lock(&at->lock);
    while (!at->done) {
        if (pthread_cond_timedwait(&at->cond, &at->lock, &ts) == ETIMEDOUT) {
            break;
        }
    }

    if (!at->done) {
        rc = -2;
    } else {
        rc = at->result;
        if (resp && resp_len) {
            snprintf(resp, resp_len, "%s", at->resp);
        }
    }

    at->pending = 0;
    pthread_mutex_unlock(&at->lock);

    return rc;
}

int quectel_at_command(struct quectel_at* at, const char* cmd, int timeout_ms, char* resp, size_t resp_len)
{
    if (!at || !cmd) {
        return -3;
    }
    if (!at->running) {
        return -3;
    }

    /*
     * Fail fast instead of hanging for the full timeout. A command issued from
     * the reader thread can never complete: that thread is the one that would
     * parse the reply, so it would be waiting on itself.
     */
    if (at->reader_started && pthread_equal(pthread_self(), at->reader)) {
        at_log(at, 3, "refusing '%s': AT commands cannot be issued from the reader thread", cmd);
        return -3;
    }

    if (resp && resp_len) {
        resp[0] = '\0';
    }

    pthread_mutex_lock(&at->cmd_lock);

    pthread_mutex_lock(&at->lock);
    at->pending   = 1;
    at->done      = 0;
    at->result    = 0;
    at->resp_used = 0;
    at->resp[0]   = '\0';
    pthread_mutex_unlock(&at->lock);

    at_log(at, 7, "> %s", cmd);

    char out[AT_LINE_MAX];
    const int n = snprintf(out, sizeof(out), "%s\r", cmd);
    if (n < 0 || (size_t)n >= sizeof(out) || write_all(at->fd, out, (size_t)n) < 0) {
        pthread_mutex_lock(&at->lock);
        at->pending = 0;
        pthread_mutex_unlock(&at->lock);
        pthread_mutex_unlock(&at->cmd_lock);
        return -3;
    }

    const int rc = wait_for_final(at, timeout_ms, resp, resp_len);
    if (rc == -2) {
        at_log(at, 4, "timeout waiting for response to '%s'", cmd);
    }
    pthread_mutex_unlock(&at->cmd_lock);

    return rc;
}

int quectel_at_arm(struct quectel_at* at)
{
    if (!at || !at->running) {
        return -3;
    }

    pthread_mutex_lock(&at->cmd_lock);

    pthread_mutex_lock(&at->lock);
    at->pending   = 1;
    at->done      = 0;
    at->result    = 0;
    at->resp_used = 0;
    at->resp[0]   = '\0';
    pthread_mutex_unlock(&at->lock);

    return 0;
}

int quectel_at_await(struct quectel_at* at, int timeout_ms, char* resp, size_t resp_len)
{
    if (!at) {
        return -3;
    }

    const int rc = wait_for_final(at, timeout_ms, resp, resp_len);
    pthread_mutex_unlock(&at->cmd_lock);

    return rc;
}

int quectel_at_send_async(struct quectel_at* at, const char* cmd)
{
    if (!at || !cmd || !at->running) {
        return -3;
    }

    char out[AT_LINE_MAX];
    const int n = snprintf(out, sizeof(out), "%s\r", cmd);
    if (n < 0 || (size_t)n >= sizeof(out)) {
        return -3;
    }

    at_log(at, 7, "> %s (async)", cmd);
    return write_all(at->fd, out, (size_t)n) < 0 ? -3 : 0;
}

int quectel_at_write_raw(struct quectel_at* at, const void* buf, size_t len)
{
    if (!at || !buf || !at->running) {
        return -3;
    }
    return write_all(at->fd, buf, len) < 0 ? -3 : 0;
}
