/*
 * chan_quectel.c - Asterisk channel driver for the Quectel EC25-E
 *
 * Deliberately supports exactly one thing: an EC25-E carrying voice over its
 * USB Audio Class interface, with AT commands on a separate serial port. No
 * SimCOM variants, no PCM-over-serial, no multiparty, no analog codecs.
 */

#include "quectel.h"

#include <asterisk/causes.h>
#include <asterisk/cli.h>
#include <asterisk/config.h>
#include <asterisk/format_cache.h>
#include <asterisk/module.h>
#include <asterisk/musiconhold.h>
#include <asterisk/pbx.h>
#include <asterisk/stasis_channels.h>
#include <asterisk/utils.h>

#include <signal.h>

#ifndef CHAN_QUECTEL_VERSION
#define CHAN_QUECTEL_VERSION "unknown"
#endif

struct quectel_devices quectel_devices = AST_RWLIST_HEAD_INIT_VALUE;

static struct ast_channel* quectel_request(const char* type, struct ast_format_cap* cap, const struct ast_assigned_ids* assignedids,
                                        const struct ast_channel* requestor, const char* data, int* cause);
static int quectel_call(struct ast_channel* chan, const char* dest, int timeout);
static int quectel_hangup(struct ast_channel* chan);
static int quectel_answer(struct ast_channel* chan);
static struct ast_frame* quectel_read(struct ast_channel* chan);
static int quectel_write(struct ast_channel* chan, struct ast_frame* frame);
static int quectel_fixup(struct ast_channel* oldchan, struct ast_channel* newchan);
static int quectel_indicate(struct ast_channel* chan, int cond, const void* data, size_t datalen);
static int quectel_digit_end(struct ast_channel* chan, char digit, unsigned int duration);

static struct ast_channel_tech quectel_tech = {
    .type            = QUECTEL_CHANNEL_TYPE,
    .description     = "Quectel EC25-E",
    .requester       = quectel_request,
    .call            = quectel_call,
    .hangup          = quectel_hangup,
    .answer          = quectel_answer,
    .read            = quectel_read,
    .write           = quectel_write,
    .fixup           = quectel_fixup,
    .indicate        = quectel_indicate,
    .send_digit_end  = quectel_digit_end,
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

const char* quectel_dev_state_str(enum quectel_dev_state s)
{
    switch (s) {
        case QUECTEL_DEV_STOPPED:    return "Stopped";
        case QUECTEL_DEV_CONNECTING: return "Connecting";
        case QUECTEL_DEV_READY:      return "Ready";
    }
    return "Unknown";
}

const char* quectel_call_state_str(enum quectel_call_state s)
{
    switch (s) {
        case QUECTEL_CALL_IDLE:     return "Idle";
        case QUECTEL_CALL_INCOMING: return "Incoming";
        case QUECTEL_CALL_DIALING:  return "Dialing";
        case QUECTEL_CALL_ACTIVE:   return "Active";
    }
    return "Unknown";
}

struct quectel_pvt* quectel_find_device(const char* name)
{
    struct quectel_pvt* pvt;

    AST_RWLIST_TRAVERSE(&quectel_devices, pvt, entry) {
        if (!strcasecmp(pvt->id, name)) {
            return pvt;
        }
    }
    return NULL;
}

struct quectel_pvt* quectel_find_free_device(int group)
{
    struct quectel_pvt* pvt;

    AST_RWLIST_TRAVERSE(&quectel_devices, pvt, entry) {
        ast_mutex_lock(&pvt->lock);
        const int usable = (pvt->state == QUECTEL_DEV_READY) && (pvt->call_state == QUECTEL_CALL_IDLE) && (group < 0 || pvt->group == group);
        ast_mutex_unlock(&pvt->lock);
        if (usable) {
            return pvt;
        }
    }
    return NULL;
}

/* Logging shims so at.c / audio.c can stay Asterisk-free. */
static void quectel_log_cb(int level, const char* msg)
{
    switch (level) {
        case 3:  ast_log(LOG_ERROR, "%s\n", msg); break;
        case 4:  ast_log(LOG_WARNING, "%s\n", msg); break;
        case 6:  ast_log(LOG_NOTICE, "%s\n", msg); break;
        default: ast_debug(3, "%s\n", msg); break;
    }
}

/* ------------------------------------------------------------------ */
/* call handling                                                       */
/* ------------------------------------------------------------------ */

void quectel_hangup_call(struct quectel_pvt* pvt)
{
    if (pvt->at) {
        /* Asynchronous: never block a channel callback on the serial port. */
        quectel_at_send_async(pvt->at, "AT+CHUP");
    }
    pvt->call_state = QUECTEL_CALL_IDLE;
    pvt->cid_num[0] = '\0';
    if (pvt->audio) {
        quectel_audio_flush(pvt->audio);
    }
}

static struct ast_channel* quectel_new_channel(struct quectel_pvt* pvt, int state, const char* cid_num, const struct ast_assigned_ids* assignedids,
                                            const struct ast_channel* requestor)
{
    struct ast_channel* chan = ast_channel_alloc(1, state, cid_num, pvt->id, NULL, pvt->exten, pvt->context, assignedids, requestor, 0, "%s/%s-%04lx",
                                                 QUECTEL_CHANNEL_TYPE, pvt->id, (unsigned long)ast_random() & 0xffff);
    if (!chan) {
        return NULL;
    }

    ast_channel_tech_set(chan, &quectel_tech);
    ast_channel_tech_pvt_set(chan, pvt);

    struct ast_format_cap* const caps = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
    if (caps) {
        ast_format_cap_append(caps, ast_format_slin, 0);
        ast_channel_nativeformats_set(chan, caps);
        ao2_ref(caps, -1);
    }
    ast_channel_set_writeformat(chan, ast_format_slin);
    ast_channel_set_readformat(chan, ast_format_slin);
    ast_channel_set_rawwriteformat(chan, ast_format_slin);
    ast_channel_set_rawreadformat(chan, ast_format_slin);

    if (!ast_strlen_zero(pvt->language)) {
        ast_channel_language_set(chan, pvt->language);
    }

    /*
     * Bring the audio stream up for the duration of the call, then hand its
     * eventfd to the channel as the readability source. That eventfd is the
     * whole reason this driver exists - see audio.h.
     */
    if (pvt->audio) {
        if (quectel_audio_start(pvt->audio)) {
            ast_log(LOG_ERROR, "[%s] cannot start audio, call will have no media\n", pvt->id);
        } else {
            ast_channel_set_fd(chan, 0, quectel_audio_fd(pvt->audio));
            quectel_audio_flush(pvt->audio);
        }
    }

    pvt->owner = chan;
    ast_module_ref(ast_module_info->self);
    ast_channel_unlock(chan);

    return chan;
}

/* Called from the device worker thread when the modem reports an inbound call. */
static void quectel_handle_incoming(struct quectel_pvt* pvt)
{
    if (pvt->owner) {
        return; /* already have a call - EC25-E here is single-call only */
    }

    pvt->call_state = QUECTEL_CALL_INCOMING;
    pvt->stats.calls_in++;

    struct ast_channel* const chan = quectel_new_channel(pvt, AST_STATE_RING, pvt->cid_num, NULL, NULL);
    if (!chan) {
        ast_log(LOG_ERROR, "[%s] cannot allocate channel for incoming call\n", pvt->id);
        quectel_hangup_call(pvt);
        return;
    }

    if (ast_pbx_start(chan)) {
        ast_log(LOG_ERROR, "[%s] cannot start PBX on %s\n", pvt->id, ast_channel_name(chan));
        pvt->owner = NULL;
        ast_hangup(chan);
        quectel_hangup_call(pvt);
    }
}

static struct ast_channel* quectel_request(const char* type, struct ast_format_cap* cap, const struct ast_assigned_ids* assignedids,
                                        const struct ast_channel* requestor, const char* data, int* cause)
{
    if (ast_strlen_zero(data)) {
        ast_log(LOG_WARNING, "Quectel dial string is empty - use Quectel/<device>/<number> or Quectel/g<group>/<number>\n");
        *cause = AST_CAUSE_INVALID_NUMBER_FORMAT;
        return NULL;
    }

    char buf[256];
    ast_copy_string(buf, data, sizeof(buf));

    char* number = strchr(buf, '/');
    if (!number) {
        ast_log(LOG_WARNING, "Quectel dial string '%s' has no number part\n", data);
        *cause = AST_CAUSE_INVALID_NUMBER_FORMAT;
        return NULL;
    }
    *number++ = '\0';

    /* AST_FORMAT_CMP_EQUAL is 0, so compare explicitly rather than with '!'. */
    if (cap && ast_format_cap_iscompatible_format(cap, ast_format_slin) == AST_FORMAT_CMP_NOT_EQUAL) {
        struct ast_str* have = ast_str_alloca(256);
        ast_log(LOG_WARNING, "Quectel only offers slin, caller asked for %s\n", ast_format_cap_get_names(cap, &have));
        *cause = AST_CAUSE_BEARERCAPABILITY_NOTAVAIL;
        return NULL;
    }

    AST_RWLIST_RDLOCK(&quectel_devices);

    struct quectel_pvt* pvt = NULL;
    if (buf[0] == 'g' && isdigit((unsigned char)buf[1])) {
        pvt = quectel_find_free_device(atoi(buf + 1));
    } else {
        pvt = quectel_find_device(buf);
        if (pvt) {
            ast_mutex_lock(&pvt->lock);
            const int busy = (pvt->state != QUECTEL_DEV_READY) || (pvt->call_state != QUECTEL_CALL_IDLE);
            ast_mutex_unlock(&pvt->lock);
            if (busy) {
                pvt = NULL;
                *cause = AST_CAUSE_USER_BUSY;
            }
        }
    }

    if (!pvt) {
        AST_RWLIST_UNLOCK(&quectel_devices);
        if (!*cause) {
            *cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
        }
        return NULL;
    }

    ast_mutex_lock(&pvt->lock);
    struct ast_channel* const chan = quectel_new_channel(pvt, AST_STATE_DOWN, number, assignedids, requestor);
    ast_mutex_unlock(&pvt->lock);

    AST_RWLIST_UNLOCK(&quectel_devices);

    if (!chan) {
        *cause = AST_CAUSE_CONGESTION;
    }
    return chan;
}

static int quectel_call(struct ast_channel* chan, const char* dest, int timeout)
{
    struct quectel_pvt* const pvt = ast_channel_tech_pvt(chan);

    if (!pvt) {
        return -1;
    }
    if (ast_channel_state(chan) != AST_STATE_DOWN && ast_channel_state(chan) != AST_STATE_RESERVED) {
        ast_log(LOG_WARNING, "[%s] quectel_call called on a channel that is already up\n", pvt->id);
        return -1;
    }

    /* The number was stashed as the channel's caller id by quectel_request(). */
    const char* number = ast_channel_caller(chan)->id.number.str;
    if (ast_strlen_zero(number)) {
        return -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ATD%s;", number);

    ast_mutex_lock(&pvt->lock);
    pvt->call_state = QUECTEL_CALL_DIALING;
    pvt->stats.calls_out++;
    ast_mutex_unlock(&pvt->lock);

    /* Unlocked: see the note in quectel_answer(). */
    const int rc = quectel_at_command(pvt->at, cmd, 20000, NULL, 0);

    if (rc != 0) {
        ast_log(LOG_WARNING, "[%s] dial to %s rejected by modem (rc=%d)\n", pvt->id, number, rc);
        ast_mutex_lock(&pvt->lock);
        pvt->call_state = QUECTEL_CALL_IDLE;
        ast_mutex_unlock(&pvt->lock);
        return -1;
    }

    ast_setstate(chan, AST_STATE_DIALING);
    return 0;
}

static int quectel_answer(struct ast_channel* chan)
{
    struct quectel_pvt* const pvt = ast_channel_tech_pvt(chan);

    if (!pvt) {
        return -1;
    }

    /*
     * Do NOT hold pvt->lock across an AT command. The reader thread needs that
     * same lock to dispatch URCs, so holding it here blocks the very thread
     * that would parse this command's reply.
     */
    const int rc = quectel_at_command(pvt->at, "ATA", 10000, NULL, 0);

    ast_mutex_lock(&pvt->lock);
    if (rc == 0) {
        pvt->call_state = QUECTEL_CALL_ACTIVE;
        pvt->stats.calls_answered++;
        quectel_audio_flush(pvt->audio);
    }
    ast_mutex_unlock(&pvt->lock);

    return rc == 0 ? 0 : -1;
}

static int quectel_hangup(struct ast_channel* chan)
{
    struct quectel_pvt* const pvt = ast_channel_tech_pvt(chan);

    if (!pvt) {
        return 0;
    }

    ast_mutex_lock(&pvt->lock);
    const int was_owner = (pvt->owner == chan);
    if (was_owner) {
        pvt->owner = NULL;
        quectel_hangup_call(pvt);
    }
    ast_mutex_unlock(&pvt->lock);

    /*
     * Stop the audio stream now the call is over. Done here, on the PBX thread,
     * rather than in quectel_hangup_call() - that also runs on the AT reader
     * thread, which must not block joining the audio thread.
     */
    if (was_owner && pvt->audio) {
        quectel_audio_stop(pvt->audio);
    }

    ast_channel_tech_pvt_set(chan, NULL);
    ast_module_unref(ast_module_info->self);

    return 0;
}

static struct ast_frame* quectel_read(struct ast_channel* chan)
{
    struct quectel_pvt* const pvt = ast_channel_tech_pvt(chan);

    /*
     * Per-thread storage: Asterisk consumes the frame before this channel is
     * read again, so there is no need to allocate one per frame.
     */
    static __thread struct ast_frame frame;
    static __thread char fbuf[AST_FRIENDLY_OFFSET + QUECTEL_FRAME_BYTES];

    if (!pvt || !pvt->audio) {
        return &ast_null_frame;
    }

    memset(&frame, 0, sizeof(frame));
    frame.frametype       = AST_FRAME_VOICE;
    frame.subclass.format = ast_format_slin;
    frame.src             = QUECTEL_CHANNEL_TYPE;
    frame.offset          = AST_FRIENDLY_OFFSET;
    frame.data.ptr        = fbuf + AST_FRIENDLY_OFFSET;
    frame.datalen         = QUECTEL_FRAME_BYTES;
    frame.samples         = QUECTEL_FRAME_SAMPLES;

    if (!quectel_audio_read(pvt->audio, fbuf + AST_FRIENDLY_OFFSET)) {
        /* Nothing buffered yet - hand back silence to keep timing honest. */
        memset(fbuf + AST_FRIENDLY_OFFSET, 0, QUECTEL_FRAME_BYTES);
    }

    return &frame;
}

static int quectel_write(struct ast_channel* chan, struct ast_frame* frame)
{
    struct quectel_pvt* const pvt = ast_channel_tech_pvt(chan);

    if (!pvt || !pvt->audio) {
        return 0;
    }
    if (frame->frametype != AST_FRAME_VOICE || !frame->datalen) {
        return 0;
    }
    if (ast_format_cmp(frame->subclass.format, ast_format_slin) != AST_FORMAT_CMP_EQUAL) {
        return 0;
    }

    quectel_audio_write(pvt->audio, frame->data.ptr, frame->datalen);
    return 0;
}

static int quectel_fixup(struct ast_channel* oldchan, struct ast_channel* newchan)
{
    struct quectel_pvt* const pvt = ast_channel_tech_pvt(newchan);

    if (!pvt) {
        return -1;
    }

    ast_mutex_lock(&pvt->lock);
    if (pvt->owner == oldchan) {
        pvt->owner = newchan;
    }
    ast_mutex_unlock(&pvt->lock);

    return 0;
}

static int quectel_indicate(struct ast_channel* chan, int cond, const void* data, size_t datalen)
{
    switch (cond) {
        case AST_CONTROL_BUSY:
        case AST_CONTROL_CONGESTION:
        case AST_CONTROL_RINGING:
        case AST_CONTROL_PROGRESS:
        case AST_CONTROL_PROCEEDING:
        case AST_CONTROL_VIDUPDATE:
        case AST_CONTROL_SRCUPDATE:
        case AST_CONTROL_SRCCHANGE:
        case -1:
            return -1; /* let Asterisk generate tones locally */

        case AST_CONTROL_HOLD:
            ast_moh_start(chan, data, NULL);
            return 0;

        case AST_CONTROL_UNHOLD:
            ast_moh_stop(chan);
            return 0;

        default:
            return -1;
    }
}

static int quectel_digit_end(struct ast_channel* chan, char digit, unsigned int duration)
{
    struct quectel_pvt* const pvt = ast_channel_tech_pvt(chan);

    if (!pvt) {
        return -1;
    }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+VTS=%c", digit);

    quectel_at_send_async(pvt->at, cmd); /* async write, no lock needed */

    return 0;
}

/* ------------------------------------------------------------------ */
/* device worker                                                       */
/* ------------------------------------------------------------------ */

/* Extract the number from +CLIP: "+15555550123",129,... */
static void parse_clip(struct quectel_pvt* pvt, const char* line)
{
    const char* p = strchr(line, '"');
    if (!p) {
        return;
    }
    p++;
    const char* end = strchr(p, '"');
    if (!end) {
        return;
    }
    const size_t len = (size_t)(end - p);
    if (len < sizeof(pvt->cid_num)) {
        memcpy(pvt->cid_num, p, len);
        pvt->cid_num[len] = '\0';
    }
}

static void quectel_urc(void* userdata, const char* line)
{
    struct quectel_pvt* const pvt = userdata;

    ast_mutex_lock(&pvt->lock);

    if (!strncmp(line, "+CLIP:", 6)) {
        parse_clip(pvt, line);
        if (pvt->call_state == QUECTEL_CALL_IDLE && !pvt->owner) {
            quectel_handle_incoming(pvt);
        }
    } else if (!strcmp(line, "RING")) {
        /* +CLIP normally follows and carries the number; wait for it. */
        if (pvt->call_state == QUECTEL_CALL_IDLE) {
            pvt->call_state = QUECTEL_CALL_INCOMING;
        }
    } else if (!strcmp(line, "NO CARRIER") || !strncmp(line, "+QIND: \"ccinfo\"", 15)) {
        /*
         * +QIND: "ccinfo",<idx>,<dir>,<state>,<mode>,<mpty>,<number>,<type>
         *
         * Parse <state> by position. Substring matching does not work here:
         * the alerting URC ...,2,0,3,0,0,... contains ",0,0," in its mode and
         * mpty fields, so a naive search answers the channel while the far end
         * is still ringing - and everything played before the media path comes
         * up is lost.
         *
         * state: 0 = active, 2 = dialing, 3 = alerting, 4 = incoming, -1 = ended
         */
        int ended = !strcmp(line, "NO CARRIER");
        int answered = 0;

        const char* const tag = strstr(line, "\"ccinfo\"");
        if (tag) {
            int idx = 0, dir = 0, state = 99;
            if (sscanf(tag + 8, ",%d,%d,%d", &idx, &dir, &state) == 3) {
                ended    = ended || (state == -1);
                answered = (state == 0);
            }
        }

        if (ended) {
            if (pvt->owner) {
                ast_queue_hangup(pvt->owner);
            }
            pvt->call_state = QUECTEL_CALL_IDLE;
            pvt->cid_num[0] = '\0';
        } else if (answered && pvt->call_state == QUECTEL_CALL_DIALING) {
            pvt->call_state = QUECTEL_CALL_ACTIVE;
            if (pvt->owner) {
                ast_queue_control(pvt->owner, AST_CONTROL_ANSWER);
            }
        }
    } else if (!strncmp(line, "+CMTI:", 6) || !strncmp(line, "+CMT:", 5)) {
        quectel_sms_handle_urc(pvt, line);
    }

    ast_mutex_unlock(&pvt->lock);
}

static void quectel_port_failed(void* userdata)
{
    struct quectel_pvt* const pvt = userdata;

    if (pvt->shutting_down) {
        return; /* expected during teardown - do not touch the device again */
    }

    ast_log(LOG_WARNING, "[%s] AT port lost\n", pvt->id);
    ast_mutex_lock(&pvt->lock);
    pvt->state = QUECTEL_DEV_STOPPED;
    if (pvt->owner) {
        ast_queue_hangup(pvt->owner);
    }
    ast_mutex_unlock(&pvt->lock);
}

/* Pull a quoted or bare field out of a "+CMD: value" response. */
static void grab_value(const char* resp, const char* prefix, char* out, size_t outlen)
{
    const char* p = strstr(resp, prefix);
    if (!p) {
        return;
    }
    p += strlen(prefix);
    while (*p == ' ') {
        p++;
    }

    if (*p == '"') {
        p++;
        const char* end = strchr(p, '"');
        if (end) {
            const size_t len = (size_t)(end - p) < outlen - 1 ? (size_t)(end - p) : outlen - 1;
            memcpy(out, p, len);
            out[len] = '\0';
        }
        return;
    }

    size_t i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && p[i] != ',' && i < outlen - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

static int quectel_init_modem(struct quectel_pvt* pvt)
{
    char resp[512];
    struct quectel_at* const at = pvt->at;

    static const char* const setup[] = {
        "ATE0",                      /* no echo - the parser assumes it       */
        "AT+CMEE=1",                 /* numeric error codes                   */
        "AT+CLIP=1",                 /* caller id on inbound calls            */
        "AT+COLP=0",
        "AT+CVHU=0",                 /* ATH always drops the call             */
        "AT+QINDCFG=\"ccinfo\",1",   /* call state change URCs                */
        "AT+CMGF=0",                 /* SMS PDU mode - text mode cannot do    */
                                     /* UCS-2 or concatenated messages        */
        "AT+CNMI=2,1,0,2,0",         /* notify on new SMS, report delivery    */
        NULL,
    };

    if (pvt->resetmodem) {
        quectel_at_command(at, "ATZ", 5000, NULL, 0);
    }

    for (int i = 0; setup[i]; i++) {
        if (quectel_at_command(at, setup[i], 5000, NULL, 0) == -3) {
            return -1;
        }
    }

    /*
     * The two settings that actually make audio work. Without QPCMV=1,2 the UAC
     * interface streams pure silence; the stock QRXGAIN of 8192 is around 44 dB
     * too quiet. Both are worth failing loudly over.
     */
    if (quectel_at_command(at, "AT+QPCMV=1,2", 5000, NULL, 0) != 0) {
        ast_log(LOG_ERROR, "[%s] cannot enable USB audio (AT+QPCMV=1,2) - there will be no audio\n", pvt->id);
        return -1;
    }

    char cmd[64];
    if (pvt->rxgain >= 0) {
        snprintf(cmd, sizeof(cmd), "AT+QRXGAIN=%d", pvt->rxgain);
        if (quectel_at_command(at, cmd, 5000, NULL, 0) != 0) {
            ast_log(LOG_WARNING, "[%s] cannot set rxgain to %d\n", pvt->id, pvt->rxgain);
        }
    }
    if (pvt->txgain >= 0) {
        snprintf(cmd, sizeof(cmd), "AT+QMIC=%d,%d", pvt->txgain, pvt->txgain);
        if (quectel_at_command(at, cmd, 5000, NULL, 0) != 0) {
            ast_log(LOG_WARNING, "[%s] cannot set txgain to %d\n", pvt->id, pvt->txgain);
        }
    }
    quectel_at_command(at, "AT+CLVL=5", 5000, NULL, 0);

    /* Identity - purely informational, shown by the CLI. */
    if (quectel_at_command(at, "AT+CGMM", 5000, resp, sizeof(resp)) == 0) {
        ast_copy_string(pvt->model, resp, sizeof(pvt->model));
    }
    if (quectel_at_command(at, "AT+CGMR", 5000, resp, sizeof(resp)) == 0) {
        ast_copy_string(pvt->firmware, resp, sizeof(pvt->firmware));
    }
    if (quectel_at_command(at, "AT+CGSN", 5000, resp, sizeof(resp)) == 0) {
        ast_copy_string(pvt->imei, resp, sizeof(pvt->imei));
    }
    if (quectel_at_command(at, "AT+CIMI", 5000, resp, sizeof(resp)) == 0) {
        ast_copy_string(pvt->imsi, resp, sizeof(pvt->imsi));
    }
    if (quectel_at_command(at, "AT+CSCA?", 5000, resp, sizeof(resp)) == 0) {
        grab_value(resp, "+CSCA:", pvt->smsc, sizeof(pvt->smsc));
    }
    if (quectel_at_command(at, "AT+CNUM", 5000, resp, sizeof(resp)) == 0) {
        grab_value(resp, ",", pvt->number, sizeof(pvt->number));
    }

    return 0;
}

static void quectel_poll_status(struct quectel_pvt* pvt)
{
    char resp[256];

    if (quectel_at_command(pvt->at, "AT+CSQ", 5000, resp, sizeof(resp)) == 0) {
        int rssi = 0, ber = 0;
        if (sscanf(resp, "+CSQ: %d,%d", &rssi, &ber) >= 1) {
            ast_mutex_lock(&pvt->lock);
            pvt->rssi = (rssi == 99) ? 0 : rssi;
            ast_mutex_unlock(&pvt->lock);
        }
    }

    if (quectel_at_command(pvt->at, "AT+COPS?", 5000, resp, sizeof(resp)) == 0) {
        char provider[64] = "";
        grab_value(resp, "+COPS:", provider, sizeof(provider));
        const char* q = strchr(resp, '"');
        if (q) {
            q++;
            const char* end = strchr(q, '"');
            if (end && (size_t)(end - q) < sizeof(pvt->provider)) {
                ast_mutex_lock(&pvt->lock);
                memcpy(pvt->provider, q, (size_t)(end - q));
                pvt->provider[end - q] = '\0';
                pvt->registered        = 1;
                ast_mutex_unlock(&pvt->lock);
            }
        }
    }
}

static void* quectel_worker(void* arg)
{
    struct quectel_pvt* const pvt = arg;

    while (!pvt->worker_stop) {
        ast_mutex_lock(&pvt->lock);
        const enum quectel_dev_state state = pvt->state;
        ast_mutex_unlock(&pvt->lock);

        if (state != QUECTEL_DEV_READY) {
            /* (Re)connect. */
            if (pvt->at) {
                quectel_at_close(pvt->at);
                pvt->at = NULL;
            }
            if (pvt->audio) {
                quectel_audio_stop(pvt->audio);
            }

            ast_mutex_lock(&pvt->lock);
            pvt->state = QUECTEL_DEV_CONNECTING;
            ast_mutex_unlock(&pvt->lock);

            pvt->at = quectel_at_open(pvt->data_tty, quectel_log_cb, quectel_urc, quectel_port_failed, pvt);
            if (!pvt->at) {
                sleep(5);
                continue;
            }

            if (quectel_init_modem(pvt) < 0) {
                ast_log(LOG_WARNING, "[%s] modem initialisation failed, retrying\n", pvt->id);
                quectel_at_close(pvt->at);
                pvt->at = NULL;
                sleep(5);
                continue;
            }

            if (!pvt->audio) {
                pvt->audio = quectel_audio_create(pvt->alsadev, QUECTEL_RATE, QUECTEL_FRAME_SAMPLES, quectel_log_cb);
            }

            /*
             * Probe the sound device once here so a bad alsadev is reported at
             * startup rather than on the first call, then stop it again - the
             * stream only runs while a call is up.
             */
            if (!pvt->audio || quectel_audio_start(pvt->audio) < 0) {
                ast_log(LOG_ERROR, "[%s] cannot open audio device %s\n", pvt->id, pvt->alsadev);
                quectel_at_close(pvt->at);
                pvt->at = NULL;
                sleep(5);
                continue;
            }
            quectel_audio_stop(pvt->audio);

            ast_mutex_lock(&pvt->lock);
            pvt->state = QUECTEL_DEV_READY;
            ast_mutex_unlock(&pvt->lock);

            ast_log(LOG_NOTICE, "[%s] ready - %s %s, IMEI %s\n", pvt->id, pvt->model, pvt->firmware, pvt->imei);
            quectel_poll_status(pvt);
            quectel_sms_sweep_stored(pvt);
            continue;
        }

        /* Idle: drain queued SMS every second, refresh status every ten. */
        for (int i = 0; i < 10 && !pvt->worker_stop; i++) {
            sleep(1);
            quectel_sms_process_pending(pvt);
        }
        if (!pvt->worker_stop) {
            quectel_poll_status(pvt);
        }
    }

    return NULL;
}

int quectel_device_start(struct quectel_pvt* pvt)
{
    if (pvt->worker_started || pvt->disabled) {
        return 0;
    }

    pvt->worker_stop = 0;
    if (ast_pthread_create_background(&pvt->worker, NULL, quectel_worker, pvt)) {
        ast_log(LOG_ERROR, "[%s] cannot start worker thread\n", pvt->id);
        return -1;
    }
    pvt->worker_started = 1;

    return 0;
}

void quectel_device_stop(struct quectel_pvt* pvt)
{
    pvt->worker_stop = 1;

    if (pvt->worker_started) {
        pthread_join(pvt->worker, NULL);
        pvt->worker_started = 0;
    }

    ast_mutex_lock(&pvt->lock);
    if (pvt->owner) {
        ast_queue_hangup(pvt->owner);
    }
    ast_mutex_unlock(&pvt->lock);

    if (pvt->at) {
        /* Leave the modem with USB audio off, as we found it. */
        quectel_at_command(pvt->at, "AT+QPCMV=0", 2000, NULL, 0);
        quectel_at_close(pvt->at);
        pvt->at = NULL;
    }
    if (pvt->audio) {
        quectel_audio_destroy(pvt->audio);
        pvt->audio = NULL;
    }

    pvt->state = QUECTEL_DEV_STOPPED;
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static void quectel_set_defaults(struct quectel_pvt* pvt)
{
    ast_copy_string(pvt->data_tty, "/dev/ttyUSB2", sizeof(pvt->data_tty));
    ast_copy_string(pvt->alsadev, "hw:Android", sizeof(pvt->alsadev));
    ast_copy_string(pvt->context, "default", sizeof(pvt->context));
    ast_copy_string(pvt->exten, "s", sizeof(pvt->exten));
    ast_copy_string(pvt->language, "en", sizeof(pvt->language));
    pvt->group      = 0;
    pvt->rxgain        = 65535; /* the stock 8192 is far too quiet for UAC */
    pvt->txgain        = -1;
    pvt->resetmodem    = 0;
    pvt->autodeletesms = 1;
}

static int quectel_load_config(void)
{
    struct ast_config* cfg;
    struct ast_flags flags = {0};

    cfg = ast_config_load(QUECTEL_CONFIG_FILE, flags);
    if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
        ast_log(LOG_ERROR, "cannot load %s\n", QUECTEL_CONFIG_FILE);
        return -1;
    }

    const char* cat = NULL;
    while ((cat = ast_category_browse(cfg, cat))) {
        if (!strcasecmp(cat, "general")) {
            continue;
        }

        struct quectel_pvt* const pvt = ast_calloc(1, sizeof(*pvt));
        if (!pvt) {
            continue;
        }

        ast_mutex_init(&pvt->lock);
        ast_copy_string(pvt->id, cat, sizeof(pvt->id));
        quectel_set_defaults(pvt);

        for (struct ast_variable* v = ast_variable_browse(cfg, cat); v; v = v->next) {
            if (!strcasecmp(v->name, "data")) {
                ast_copy_string(pvt->data_tty, v->value, sizeof(pvt->data_tty));
            } else if (!strcasecmp(v->name, "alsadev")) {
                ast_copy_string(pvt->alsadev, v->value, sizeof(pvt->alsadev));
            } else if (!strcasecmp(v->name, "context")) {
                ast_copy_string(pvt->context, v->value, sizeof(pvt->context));
            } else if (!strcasecmp(v->name, "exten")) {
                ast_copy_string(pvt->exten, v->value, sizeof(pvt->exten));
            } else if (!strcasecmp(v->name, "language")) {
                ast_copy_string(pvt->language, v->value, sizeof(pvt->language));
            } else if (!strcasecmp(v->name, "group")) {
                pvt->group = atoi(v->value);
            } else if (!strcasecmp(v->name, "rxgain")) {
                pvt->rxgain = atoi(v->value);
            } else if (!strcasecmp(v->name, "txgain")) {
                pvt->txgain = atoi(v->value);
            } else if (!strcasecmp(v->name, "resetmodem")) {
                pvt->resetmodem = ast_true(v->value);
            } else if (!strcasecmp(v->name, "autodeletesms")) {
                pvt->autodeletesms = ast_true(v->value);
            } else if (!strcasecmp(v->name, "disabled")) {
                pvt->disabled = ast_true(v->value);
            } else {
                ast_log(LOG_WARNING, "[%s] unknown option '%s'\n", cat, v->name);
            }
        }

        AST_RWLIST_WRLOCK(&quectel_devices);
        AST_RWLIST_INSERT_TAIL(&quectel_devices, pvt, entry);
        AST_RWLIST_UNLOCK(&quectel_devices);

        ast_log(LOG_NOTICE, "[%s] configured: data=%s alsa=%s context=%s\n", pvt->id, pvt->data_tty, pvt->alsadev, pvt->context);
    }

    ast_config_destroy(cfg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* module                                                              */
/* ------------------------------------------------------------------ */

static int unload_module(void)
{
    struct quectel_pvt* pvt;

    /* Stop accepting new channels first, then join every thread we own before
       anything in this module can be unmapped. */
    ast_channel_unregister(&quectel_tech);
    quectel_cli_unregister();

    AST_RWLIST_WRLOCK(&quectel_devices);
    while ((pvt = AST_RWLIST_REMOVE_HEAD(&quectel_devices, entry))) {
        pvt->shutting_down = 1;
        quectel_device_stop(pvt);
        ast_mutex_destroy(&pvt->lock);
        ast_free(pvt);
    }
    AST_RWLIST_UNLOCK(&quectel_devices);

    ao2_cleanup(quectel_tech.capabilities);
    quectel_tech.capabilities = NULL;

    return 0;
}

static int load_module(void)
{
    quectel_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
    if (!quectel_tech.capabilities) {
        return AST_MODULE_LOAD_DECLINE;
    }
    ast_format_cap_append(quectel_tech.capabilities, ast_format_slin, 0);

    if (quectel_load_config()) {
        ao2_cleanup(quectel_tech.capabilities);
        quectel_tech.capabilities = NULL;
        return AST_MODULE_LOAD_DECLINE;
    }

    if (ast_channel_register(&quectel_tech)) {
        ast_log(LOG_ERROR, "cannot register channel type %s\n", QUECTEL_CHANNEL_TYPE);
        unload_module();
        return AST_MODULE_LOAD_DECLINE;
    }

    quectel_cli_register();

    ast_log(LOG_NOTICE, "chan_quectel %s loaded\n", CHAN_QUECTEL_VERSION);

    struct quectel_pvt* pvt;
    AST_RWLIST_RDLOCK(&quectel_devices);
    AST_RWLIST_TRAVERSE(&quectel_devices, pvt, entry) {
        quectel_device_start(pvt);
    }
    AST_RWLIST_UNLOCK(&quectel_devices);

    return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Quectel EC25-E Channel Driver", .support_level = AST_MODULE_SUPPORT_EXTENDED,
                .load = load_module, .unload = unload_module, .load_pri = AST_MODPRI_CHANNEL_DRIVER);
