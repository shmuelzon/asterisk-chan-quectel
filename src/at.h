/*
 * at.h - AT command transport for the EC25-E
 *
 * One reader thread owns the AT serial port. Commands are submitted from any
 * thread and complete when the modem answers OK / ERROR / +CME ERROR. Anything
 * the modem volunteers on its own (URCs: RING, +CLIP, +QIND, +CMTI, ...) is
 * handed to a callback instead.
 *
 * Like audio.h this stays free of Asterisk headers so it can be driven from a
 * test harness.
 */

#ifndef CHAN_QUECTEL_AT_H
#define CHAN_QUECTEL_AT_H

#include <stddef.h>

struct quectel_at;

typedef void (*quectel_at_log_fn)(int level, const char* msg);

/* Called on the reader thread for every unsolicited line. */
typedef void (*quectel_at_urc_fn)(void* userdata, const char* line);

/*
 * Called on the reader thread when the port dies (unplug, read error).
 * The owner should tear the device down.
 */
typedef void (*quectel_at_fail_fn)(void* userdata);

struct quectel_at* quectel_at_open(const char* tty, quectel_at_log_fn log, quectel_at_urc_fn urc, quectel_at_fail_fn fail, void* userdata);
void quectel_at_close(struct quectel_at* at);

/*
 * Send a command and wait for its final response.
 *
 * cmd           - without trailing CR, e.g. "AT+CLVL=5"
 * timeout_ms    - how long to wait for OK/ERROR
 * resp/resp_len - optional; receives the intermediate response lines joined by
 *                 '\n' (i.e. everything before the final OK)
 *
 * Returns 0 on OK, -1 on ERROR/+CME ERROR, -2 on timeout, -3 if the port is down.
 */
int quectel_at_command(struct quectel_at* at, const char* cmd, int timeout_ms, char* resp, size_t resp_len);

/*
 * Fire and forget - queues the command and returns immediately without waiting
 * for a response. Used for things like ATH where we do not want to block a
 * channel callback.
 */
int quectel_at_send_async(struct quectel_at* at, const char* cmd);

/*
 * Arm response tracking without sending anything, then wait for the modem's
 * next final response. Needed for two-stage commands such as AT+CMGS, where the
 * body is written separately and the OK arrives afterwards.
 *
 * Call quectel_at_arm() BEFORE writing the body, then quectel_at_await() to collect
 * the result. Return values match quectel_at_command().
 */
int quectel_at_arm(struct quectel_at* at);
int quectel_at_await(struct quectel_at* at, int timeout_ms, char* resp, size_t resp_len);

/*
 * Send raw bytes with no CR appended and no response tracking. Needed for the
 * SMS payload that follows AT+CMGS, which is terminated by Ctrl-Z.
 */
int quectel_at_write_raw(struct quectel_at* at, const void* buf, size_t len);

#endif /* CHAN_QUECTEL_AT_H */
