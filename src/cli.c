/*
 * cli.c - CLI commands for chan_quectel
 *
 * Kept close to the command set of asterisk-chan-quectel so muscle memory and
 * existing scripts carry over, minus everything that only applied to other
 * hardware.
 */

#include "quectel.h"

#include <asterisk/cli.h>
#include <asterisk/utils.h>

static char* complete_device(const char* word, int state)
{
    struct quectel_pvt* pvt;
    int which         = 0;
    const size_t wlen = strlen(word);
    char* res         = NULL;

    AST_RWLIST_RDLOCK(&quectel_devices);
    AST_RWLIST_TRAVERSE(&quectel_devices, pvt, entry) {
        if (!strncasecmp(word, pvt->id, wlen) && ++which > state) {
            res = ast_strdup(pvt->id);
            break;
        }
    }
    AST_RWLIST_UNLOCK(&quectel_devices);

    return res;
}

static char* cli_show_devices(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_INIT:
            e->command = "quectel show devices";
            e->usage   = "Usage: quectel show devices\n       List configured Quectel devices.\n";
            return NULL;
        case CLI_GENERATE:
            return NULL;
    }

    ast_cli(a->fd, "%-12s %-6s %-11s %-9s %-5s %-10s %-16s %s\n", "ID", "Group", "State", "Call", "RSSI", "Model", "IMEI", "Provider");

    struct quectel_pvt* pvt;
    AST_RWLIST_RDLOCK(&quectel_devices);
    AST_RWLIST_TRAVERSE(&quectel_devices, pvt, entry) {
        ast_mutex_lock(&pvt->lock);
        ast_cli(a->fd, "%-12s %-6d %-11s %-9s %-5d %-10s %-16s %s\n", pvt->id, pvt->group, quectel_dev_state_str(pvt->state), quectel_call_state_str(pvt->call_state),
                pvt->rssi, pvt->model, pvt->imei, pvt->provider);
        ast_mutex_unlock(&pvt->lock);
    }
    AST_RWLIST_UNLOCK(&quectel_devices);

    return CLI_SUCCESS;
}

static char* cli_show_device(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_INIT:
            e->command = "quectel show device";
            e->usage   = "Usage: quectel show device <device>\n       Show the full state of a device.\n";
            return NULL;
        case CLI_GENERATE:
            return (a->pos == 3) ? complete_device(a->word, a->n) : NULL;
    }

    if (a->argc != 4) {
        return CLI_SHOWUSAGE;
    }

    AST_RWLIST_RDLOCK(&quectel_devices);
    struct quectel_pvt* const pvt = quectel_find_device(a->argv[3]);
    if (!pvt) {
        AST_RWLIST_UNLOCK(&quectel_devices);
        ast_cli(a->fd, "No such device '%s'\n", a->argv[3]);
        return CLI_FAILURE;
    }

    ast_mutex_lock(&pvt->lock);
    ast_cli(a->fd, "-------------- Device --------------\n");
    ast_cli(a->fd, "  Device          : %s\n", pvt->id);
    ast_cli(a->fd, "  State           : %s\n", quectel_dev_state_str(pvt->state));
    ast_cli(a->fd, "  Call state      : %s\n", quectel_call_state_str(pvt->call_state));
    ast_cli(a->fd, "  AT port         : %s\n", pvt->data_tty);
    ast_cli(a->fd, "  Audio device    : %s\n", pvt->alsadev);
    ast_cli(a->fd, "  Context         : %s\n", pvt->context);
    ast_cli(a->fd, "  Exten           : %s\n", pvt->exten);
    ast_cli(a->fd, "  Group           : %d\n", pvt->group);
    ast_cli(a->fd, "  RX gain         : %d\n", pvt->rxgain);
    ast_cli(a->fd, "  TX gain         : %d\n", pvt->txgain);
    ast_cli(a->fd, "-------------- Modem ---------------\n");
    ast_cli(a->fd, "  Model           : %s\n", pvt->model);
    ast_cli(a->fd, "  Firmware        : %s\n", pvt->firmware);
    ast_cli(a->fd, "  IMEI            : %s\n", pvt->imei);
    ast_cli(a->fd, "  IMSI            : %s\n", pvt->imsi);
    ast_cli(a->fd, "  Number          : %s\n", pvt->number);
    ast_cli(a->fd, "  SMS centre      : %s\n", pvt->smsc);
    ast_cli(a->fd, "  Provider        : %s\n", pvt->provider);
    ast_cli(a->fd, "  Registered      : %s\n", pvt->registered ? "yes" : "no");
    ast_cli(a->fd, "  RSSI            : %d (%d dBm)\n", pvt->rssi, pvt->rssi ? (pvt->rssi * 2 - 113) : 0);

    if (pvt->audio) {
        struct quectel_audio_stats st;
        quectel_audio_get_stats(pvt->audio, &st);
        ast_cli(a->fd, "-------------- Audio ---------------\n");
        ast_cli(a->fd, "  Frames captured : %lu\n", st.frames_captured);
        ast_cli(a->fd, "  Frames played   : %lu\n", st.frames_played);
        ast_cli(a->fd, "  Capture xruns   : %lu\n", st.capture_xruns);
        ast_cli(a->fd, "  Playback xruns  : %lu\n", st.playback_xruns);
        ast_cli(a->fd, "  Capture errors  : %lu\n", st.capture_errors);
        ast_cli(a->fd, "  Playback errors : %lu\n", st.playback_errors);
        ast_cli(a->fd, "  RX dropped      : %lu bytes\n", st.rx_dropped);
        ast_cli(a->fd, "  RX underruns    : %lu\n", st.rx_underruns);
        ast_cli(a->fd, "  TX silence      : %lu frames\n", st.tx_silence);
    }

    ast_cli(a->fd, "-------------- Calls ---------------\n");
    ast_cli(a->fd, "  Incoming        : %lu\n", pvt->stats.calls_in);
    ast_cli(a->fd, "  Outgoing        : %lu\n", pvt->stats.calls_out);
    ast_cli(a->fd, "  Answered        : %lu\n", pvt->stats.calls_answered);
    ast_cli(a->fd, "  SMS received    : %lu\n", pvt->stats.sms_in);
    ast_cli(a->fd, "  SMS sent        : %lu\n", pvt->stats.sms_out);
    ast_mutex_unlock(&pvt->lock);

    AST_RWLIST_UNLOCK(&quectel_devices);
    return CLI_SUCCESS;
}

static char* cli_cmd(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_INIT:
            e->command = "quectel cmd";
            e->usage   = "Usage: quectel cmd <device> <AT command>\n       Send a raw AT command and print the reply.\n";
            return NULL;
        case CLI_GENERATE:
            return (a->pos == 2) ? complete_device(a->word, a->n) : NULL;
    }

    if (a->argc < 4) {
        return CLI_SHOWUSAGE;
    }

    AST_RWLIST_RDLOCK(&quectel_devices);
    struct quectel_pvt* const pvt = quectel_find_device(a->argv[2]);
    if (!pvt || !pvt->at) {
        AST_RWLIST_UNLOCK(&quectel_devices);
        ast_cli(a->fd, "Device '%s' is not connected\n", a->argv[2]);
        return CLI_FAILURE;
    }

    /* Re-join the remaining words so quoting is not required for simple cases. */
    char command[256] = "";
    for (int i = 3; i < a->argc; i++) {
        strncat(command, a->argv[i], sizeof(command) - strlen(command) - 2);
        if (i + 1 < a->argc) {
            strncat(command, " ", sizeof(command) - strlen(command) - 2);
        }
    }

    char resp[1024];
    const int rc = quectel_at_command(pvt->at, command, 10000, resp, sizeof(resp));
    AST_RWLIST_UNLOCK(&quectel_devices);

    ast_cli(a->fd, "%s\n", resp[0] ? resp : "(no payload)");
    ast_cli(a->fd, "%s\n", rc == 0 ? "OK" : (rc == -2 ? "TIMEOUT" : (rc == -3 ? "PORT DOWN" : "ERROR")));

    return CLI_SUCCESS;
}

static char* cli_sms_send(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_INIT:
            e->command = "quectel sms send";
            e->usage   = "Usage: quectel sms send <device> <number> <message...>\n";
            return NULL;
        case CLI_GENERATE:
            return (a->pos == 3) ? complete_device(a->word, a->n) : NULL;
    }

    if (a->argc < 6) {
        return CLI_SHOWUSAGE;
    }

    char text[512] = "";
    for (int i = 5; i < a->argc; i++) {
        strncat(text, a->argv[i], sizeof(text) - strlen(text) - 2);
        if (i + 1 < a->argc) {
            strncat(text, " ", sizeof(text) - strlen(text) - 2);
        }
    }

    AST_RWLIST_RDLOCK(&quectel_devices);
    struct quectel_pvt* const pvt = quectel_find_device(a->argv[3]);
    if (!pvt) {
        AST_RWLIST_UNLOCK(&quectel_devices);
        ast_cli(a->fd, "No such device '%s'\n", a->argv[3]);
        return CLI_FAILURE;
    }

    const int rc = quectel_sms_send(pvt, a->argv[4], text);
    AST_RWLIST_UNLOCK(&quectel_devices);

    ast_cli(a->fd, "%s\n", rc == 0 ? "Message sent" : "Send failed");
    return rc == 0 ? CLI_SUCCESS : CLI_FAILURE;
}

static char* cli_restart(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_INIT:
            e->command = "quectel restart";
            e->usage   = "Usage: quectel restart <device>\n       Reconnect to the modem.\n";
            return NULL;
        case CLI_GENERATE:
            return (a->pos == 2) ? complete_device(a->word, a->n) : NULL;
    }

    if (a->argc != 3) {
        return CLI_SHOWUSAGE;
    }

    AST_RWLIST_RDLOCK(&quectel_devices);
    struct quectel_pvt* const pvt = quectel_find_device(a->argv[2]);
    if (!pvt) {
        AST_RWLIST_UNLOCK(&quectel_devices);
        ast_cli(a->fd, "No such device '%s'\n", a->argv[2]);
        return CLI_FAILURE;
    }

    /* Dropping the state makes the worker tear down and reconnect. */
    ast_mutex_lock(&pvt->lock);
    pvt->state = QUECTEL_DEV_STOPPED;
    ast_mutex_unlock(&pvt->lock);
    AST_RWLIST_UNLOCK(&quectel_devices);

    ast_cli(a->fd, "Reconnecting %s\n", a->argv[2]);
    return CLI_SUCCESS;
}

static char* cli_audio_gain(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_INIT:
            e->command = "quectel audio gain";
            e->usage   = "Usage: quectel audio gain <device> rx|tx [<value>]\n"
                         "       Query or set the modem audio gain (0-65535).\n"
                         "       Note the EC25-E needs rx=65535 for a usable level.\n";
            return NULL;
        case CLI_GENERATE:
            return (a->pos == 3) ? complete_device(a->word, a->n) : NULL;
    }

    if (a->argc < 5) {
        return CLI_SHOWUSAGE;
    }

    const int is_rx = !strcasecmp(a->argv[4], "rx");
    if (!is_rx && strcasecmp(a->argv[4], "tx")) {
        return CLI_SHOWUSAGE;
    }

    AST_RWLIST_RDLOCK(&quectel_devices);
    struct quectel_pvt* const pvt = quectel_find_device(a->argv[3]);
    if (!pvt || !pvt->at) {
        AST_RWLIST_UNLOCK(&quectel_devices);
        ast_cli(a->fd, "Device '%s' is not connected\n", a->argv[3]);
        return CLI_FAILURE;
    }

    char command[64];
    char resp[256];

    if (a->argc >= 6) {
        const int val = atoi(a->argv[5]);
        if (is_rx) {
            snprintf(command, sizeof(command), "AT+QRXGAIN=%d", val);
        } else {
            snprintf(command, sizeof(command), "AT+QMIC=%d,%d", val, val);
        }
        const int rc = quectel_at_command(pvt->at, command, 5000, NULL, 0);
        if (rc == 0) {
            ast_mutex_lock(&pvt->lock);
            *(is_rx ? &pvt->rxgain : &pvt->txgain) = val;
            ast_mutex_unlock(&pvt->lock);
        }
        ast_cli(a->fd, "%s\n", rc == 0 ? "OK" : "Failed");
    } else {
        snprintf(command, sizeof(command), is_rx ? "AT+QRXGAIN?" : "AT+QMIC?");
        quectel_at_command(pvt->at, command, 5000, resp, sizeof(resp));
        ast_cli(a->fd, "%s\n", resp);
    }

    AST_RWLIST_UNLOCK(&quectel_devices);
    return CLI_SUCCESS;
}

static char* cli_version(struct ast_cli_entry* e, int cmd, struct ast_cli_args* a)
{
    switch (cmd) {
        case CLI_INIT:
            e->command = "quectel show version";
            e->usage   = "Usage: quectel show version\n";
            return NULL;
        case CLI_GENERATE:
            return NULL;
    }

#ifndef CHAN_QUECTEL_VERSION
#define CHAN_QUECTEL_VERSION "unknown"
#endif
    ast_cli(a->fd, "chan_quectel %s - Asterisk channel driver for Quectel modules\n", CHAN_QUECTEL_VERSION);
    return CLI_SUCCESS;
}

static struct ast_cli_entry quectel_cli[] = {
    AST_CLI_DEFINE(cli_show_devices, "List EC25 devices"),
    AST_CLI_DEFINE(cli_show_device, "Show device state"),
    AST_CLI_DEFINE(cli_version, "Show module version"),
    AST_CLI_DEFINE(cli_cmd, "Send a raw AT command"),
    AST_CLI_DEFINE(cli_sms_send, "Send an SMS"),
    AST_CLI_DEFINE(cli_restart, "Reconnect a device"),
    AST_CLI_DEFINE(cli_audio_gain, "Query/set audio gain"),
};

void quectel_cli_register(void) { ast_cli_register_multiple(quectel_cli, ARRAY_LEN(quectel_cli)); }

void quectel_cli_unregister(void) { ast_cli_unregister_multiple(quectel_cli, ARRAY_LEN(quectel_cli)); }
