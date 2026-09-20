/*
 * sms.c - SMS handling for chan_quectel, PDU mode
 *
 * The modem is kept in PDU mode (AT+CMGF=0). Text mode cannot represent
 * anything outside the GSM alphabet and cannot carry concatenated messages, so
 * it is unusable for e.g. Hebrew or anything longer than 160 characters.
 *
 * The PDU encoder/decoder and the GSM7/UCS-2 conversions are ported from
 * asterisk-chan-quectel (pdu.c, char_conv.c, gsm7_luts.h) - that code is
 * well-tested and orthogonal to the audio problems which motivated this driver.
 *
 * Incoming messages are delivered to the "sms" extension of the device's
 * context with SMS_FROM / SMS_TEXT / SMS_TS / SMS_DEVICE set. Concatenated
 * messages are reassembled first, so the dialplan always sees a whole message.
 */

#include "quectel.h"

#include <asterisk/causes.h>
#include <asterisk/format_cache.h>
#include <asterisk/localtime.h>
#include <asterisk/pbx.h>
#include <asterisk/strings.h>
#include <asterisk/utils.h>

#include "char_conv.h"
#include "error.h"
#include "pdu.h"

/* Bounded so a hostile or broken message cannot make us allocate without end. */
#define SMS_MAX_UCS2 (QUECTEL_SMS_MAX_PARTS * 67)
#define SMS_MAX_UTF8 (SMS_MAX_UCS2 * 4 + 1)
#define SMS_MAX_PDUS QUECTEL_SMS_MAX_PARTS

/* Reference used for outgoing concatenated messages. */
static unsigned char csms_ref;

/* ------------------------------------------------------------------ */
/* delivery to the dialplan                                            */
/* ------------------------------------------------------------------ */

/*
 * Render a message for a single log line.
 *
 * Asterisk's logger applies its own continuation formatting to embedded
 * newlines, and that formatting is not UTF-8 aware: it mangles the first byte
 * after the break, which looks exactly like a decoding bug. Keep log output on
 * one line by escaping control characters.
 */
static const char* log_safe(const char* text, char* buf, size_t buflen)
{
    size_t o = 0;

    for (const unsigned char* p = (const unsigned char*)S_OR(text, ""); *p && o + 3 < buflen; p++) {
        if (*p == '\n' || *p == '\r') {
            buf[o++] = '\\';
            buf[o++] = (*p == '\n') ? 'n' : 'r';
        } else if (*p == '\t') {
            buf[o++] = ' ';
        } else if (*p < 0x20) {
            buf[o++] = '?';
        } else {
            buf[o++] = (char)*p;
        }
    }

    buf[o] = '\0';
    return buf;
}

/*
 * A Local channel is requested for "sms@<context>" and then run with
 * ast_pbx_start(). The channel's address must carry the extension and context.
 *
 * Called on the device worker thread.
 */
static void sms_deliver(struct quectel_pvt* pvt, const char* from, const char* ts, const char* text)
{
    if (!ast_exists_extension(NULL, pvt->context, "sms", 1, NULL)) {
        char safe[1024];
        ast_log(LOG_NOTICE, "[%s] SMS from %s: %s (no 'sms' extension in context %s)\n", pvt->id, S_OR(from, "?"), log_safe(text, safe, sizeof(safe)),
                pvt->context);
        return;
    }

    char dest[AST_MAX_CONTEXT + AST_MAX_EXTENSION + 8];
    snprintf(dest, sizeof(dest), "sms@%s", pvt->context);

    struct ast_format_cap* const caps = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
    if (!caps) {
        return;
    }
    ast_format_cap_append(caps, ast_format_slin, 0);

    int cause                = 0;
    struct ast_channel* chan = ast_request("Local", caps, NULL, NULL, dest, &cause);
    ao2_ref(caps, -1);

    if (!chan) {
        ast_log(LOG_ERROR, "[%s] cannot request Local/%s (cause %d)\n", pvt->id, dest, cause);
        return;
    }

    pbx_builtin_setvar_helper(chan, "SMS_DEVICE", pvt->id);
    pbx_builtin_setvar_helper(chan, "SMS_FROM", S_OR(from, ""));
    pbx_builtin_setvar_helper(chan, "SMS_TEXT", S_OR(text, ""));
    pbx_builtin_setvar_helper(chan, "SMS_TS", S_OR(ts, ""));

    /* Also present the sender as caller id, so ${CALLERID(num)} works. */
    ast_set_callerid(chan, S_OR(from, ""), pvt->id, S_OR(from, ""));

    pvt->stats.sms_in++;
    char safe[1024];
    ast_log(LOG_NOTICE, "[%s] SMS from %s: %s\n", pvt->id, S_OR(from, "?"), log_safe(text, safe, sizeof(safe)));

    if (ast_pbx_start(chan)) {
        ast_log(LOG_ERROR, "[%s] cannot start pbx on Local/%s\n", pvt->id, dest);
        ast_hangup(chan);
    }
}

/* ------------------------------------------------------------------ */
/* reassembly of concatenated messages                                 */
/* ------------------------------------------------------------------ */

static void frag_release(struct quectel_sms_frag* f)
{
    for (int i = 0; i < QUECTEL_SMS_MAX_PARTS; i++) {
        ast_free(f->part[i]);
        f->part[i] = NULL;
    }
    memset(f, 0, sizeof(*f));
}

/* Drop slots whose remaining parts never turned up. */
static void frag_expire(struct quectel_pvt* pvt)
{
    const time_t now = time(NULL);

    for (int i = 0; i < QUECTEL_SMS_FRAG_SLOTS; i++) {
        struct quectel_sms_frag* const f = &pvt->frags[i];
        if (f->in_use && now - f->first_seen > QUECTEL_SMS_FRAG_TTL) {
            ast_log(LOG_WARNING, "[%s] incomplete multipart SMS from %s discarded (%d of %u parts)\n", pvt->id, f->from, f->received, f->parts);
            frag_release(f);
        }
    }
}

/*
 * Store one part. When the last outstanding part arrives the whole message is
 * joined and delivered.
 */
static void frag_add(struct quectel_pvt* pvt, const pdu_udh_t* udh, const char* from, const char* ts, const char* text)
{
    frag_expire(pvt);

    if (udh->parts > QUECTEL_SMS_MAX_PARTS || udh->order < 1 || udh->order > udh->parts) {
        ast_log(LOG_WARNING, "[%s] multipart SMS part %u/%u out of range, delivering as-is\n", pvt->id, udh->order, udh->parts);
        sms_deliver(pvt, from, ts, text);
        return;
    }

    struct quectel_sms_frag* f         = NULL;
    struct quectel_sms_frag* free_slot = NULL;

    for (int i = 0; i < QUECTEL_SMS_FRAG_SLOTS; i++) {
        struct quectel_sms_frag* const cand = &pvt->frags[i];
        if (cand->in_use && cand->ref == udh->ref && cand->parts == udh->parts && !strcmp(cand->from, S_OR(from, ""))) {
            f = cand;
            break;
        }
        if (!cand->in_use && !free_slot) {
            free_slot = cand;
        }
    }

    if (!f) {
        if (!free_slot) {
            /* All slots busy - give up the oldest rather than lose this one. */
            free_slot = &pvt->frags[0];
            for (int i = 1; i < QUECTEL_SMS_FRAG_SLOTS; i++) {
                if (pvt->frags[i].first_seen < free_slot->first_seen) {
                    free_slot = &pvt->frags[i];
                }
            }
            ast_log(LOG_WARNING, "[%s] reassembly slots exhausted, dropping partial message from %s\n", pvt->id, free_slot->from);
            frag_release(free_slot);
        }

        f             = free_slot;
        f->in_use     = 1;
        f->ref        = udh->ref;
        f->parts      = udh->parts;
        f->first_seen = time(NULL);
        ast_copy_string(f->from, S_OR(from, ""), sizeof(f->from));
        ast_copy_string(f->ts, S_OR(ts, ""), sizeof(f->ts));
    }

    const int slot = udh->order - 1;
    if (f->part[slot]) {
        ast_free(f->part[slot]); /* duplicate part - keep the newest */
        f->received--;
    }

    f->part[slot] = ast_strdup(S_OR(text, ""));
    if (!f->part[slot]) {
        return;
    }
    f->received++;

    ast_debug(2, "[%s] multipart SMS ref %u: %d of %u parts\n", pvt->id, f->ref, f->received, f->parts);

    if (f->received < f->parts) {
        return;
    }

    size_t total = 1;
    for (int i = 0; i < f->parts; i++) {
        total += f->part[i] ? strlen(f->part[i]) : 0;
    }

    char* const joined = ast_calloc(1, total);
    if (joined) {
        for (int i = 0; i < f->parts; i++) {
            if (f->part[i]) {
                strncat(joined, f->part[i], total - strlen(joined) - 1);
            }
        }
        sms_deliver(pvt, f->from, f->ts, joined);
        ast_free(joined);
    }

    frag_release(f);
}

/* ------------------------------------------------------------------ */
/* receive                                                             */
/* ------------------------------------------------------------------ */

static void format_ts(const struct ast_tm* tm, char* out, size_t outlen)
{
    if (!tm->tm_year) {
        out[0] = '\0';
        return;
    }

    char buf[64];
    ast_strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    ast_copy_string(out, buf, outlen);
}

/*
 * Decode one hex PDU as returned by AT+CMGR and act on it.
 * The buffer is modified in place - unhex() decodes onto itself.
 */
static void sms_handle_pdu(struct quectel_pvt* pvt, char* hex)
{
    const int nibbles = unhex(hex, (uint8_t*)hex);
    if (nibbles < 0) {
        ast_log(LOG_WARNING, "[%s] malformed PDU hex string\n", pvt->id);
        return;
    }

    const int pdu_len  = (nibbles + 1) / 2;
    uint8_t* const pdu = (uint8_t*)hex;

    char sca[32] = "";
    int i        = 0;

    int res = pdu_parse_sca(pdu, pdu_len, sca, sizeof(sca));
    if (res < 0) {
        ast_log(LOG_WARNING, "[%s] cannot parse SMSC address: %s\n", pvt->id, error2str(chan_quectel_err));
        return;
    }
    i += res;

    int tpdu_type = 0;
    res           = tpdu_parse_type(pdu + i, pdu_len - i, &tpdu_type);
    if (res < 0) {
        ast_log(LOG_WARNING, "[%s] cannot parse TPDU type: %s\n", pvt->id, error2str(chan_quectel_err));
        return;
    }
    i += res;

    char oa[64]        = "";
    struct ast_tm scts = {0};

    switch (PDUTYPE_MTI(tpdu_type)) {
        case PDUTYPE_MTI_SMS_DELIVER: {
            uint16_t msg16[SMS_MAX_UCS2];
            pdu_udh_t udh;
            pdu_udh_init(&udh);

            res = tpdu_parse_deliver(pdu + i, pdu_len - i, tpdu_type, oa, sizeof(oa), &scts, msg16, &udh);
            if (res < 0) {
                ast_log(LOG_WARNING, "[%s] cannot parse message: %s\n", pvt->id, error2str(chan_quectel_err));
                return;
            }

            char text[SMS_MAX_UTF8];
            const ssize_t tlen = ucs2_to_utf8(msg16, res, text, sizeof(text) - 1);
            if (tlen < 0) {
                ast_log(LOG_WARNING, "[%s] cannot convert message to UTF-8\n", pvt->id);
                return;
            }
            text[tlen] = '\0';

            char ts[64];
            format_ts(&scts, ts, sizeof(ts));

            ast_debug(1, "[%s] deliver: dcs-units=%d utf8=%zd part=%u/%u ref=%u\n", pvt->id, res, tlen, udh.order, udh.parts, udh.ref);

            if (udh.parts > 1) {
                frag_add(pvt, &udh, oa, ts, text);
            } else {
                sms_deliver(pvt, oa, ts, text);
            }
            break;
        }

        case PDUTYPE_MTI_SMS_STATUS_REPORT: {
            struct ast_tm dt = {0};
            int mr = 0, st = 0;

            res = tpdu_parse_status_report(pdu + i, pdu_len - i, &mr, oa, sizeof(oa), &scts, &dt, &st);
            if (res < 0) {
                ast_log(LOG_WARNING, "[%s] cannot parse status report: %s\n", pvt->id, error2str(chan_quectel_err));
                return;
            }

            /* Status values below 0x20 mean the message reached the handset. */
            ast_log(LOG_NOTICE, "[%s] delivery report for message %d to %s: %s (status 0x%02x)\n", pvt->id, mr, oa, (st < 0x20) ? "delivered" : "failed", st);
            break;
        }

        default:
            ast_log(LOG_NOTICE, "[%s] ignoring PDU of type 0x%02x\n", pvt->id, tpdu_type);
            break;
    }
}

/*
 * Fetch a stored message. In PDU mode AT+CMGR answers with
 *   +CMGR: <stat>,[<alpha>],<length>
 *   <hex pdu>
 */
static void sms_fetch(struct quectel_pvt* pvt, int index)
{
    char cmd[64];
    char resp[2048];

    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
    if (quectel_at_command(pvt->at, cmd, 10000, resp, sizeof(resp)) != 0) {
        ast_log(LOG_WARNING, "[%s] cannot read SMS at index %d\n", pvt->id, index);
        return;
    }

    char* hex = strchr(resp, '\n');
    if (!hex) {
        ast_log(LOG_WARNING, "[%s] CMGR reply for index %d has no PDU\n", pvt->id, index);
        return;
    }
    hex++;

    /* An empty slot is not an error worth reporting. */
    if (ast_strlen_zero(hex)) {
        return;
    }

    /* Log the raw PDU so a decoding problem can be reproduced offline. */
    ast_debug(1, "[%s] PDU[%d] %s\n", pvt->id, index, hex);

    sms_handle_pdu(pvt, hex);

    /*
     * Delete only after a successful read, so a message is never lost because
     * the fetch failed. Modem storage is small (255 slots) and nothing else
     * prunes it, so leaving this off will eventually stop delivery entirely.
     */
    if (pvt->autodeletesms) {
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
        if (quectel_at_command(pvt->at, cmd, 10000, NULL, 0) != 0) {
            ast_log(LOG_WARNING, "[%s] cannot delete message %d from storage\n", pvt->id, index);
        }
    }
}

/* Warn once storage starts running out, whatever the autodelete setting. */
static void sms_check_storage(struct quectel_pvt* pvt)
{
    char resp[256];

    if (quectel_at_command(pvt->at, "AT+CPMS?", 5000, resp, sizeof(resp)) != 0) {
        return;
    }

    int used = 0, total = 0;
    const char* const p = strstr(resp, "+CPMS:");
    if (!p) {
        return;
    }

    /* +CPMS: "ME",used,total,... */
    const char* const comma = strchr(p, ',');
    if (!comma || sscanf(comma + 1, "%d,%d", &used, &total) != 2 || total <= 0) {
        return;
    }

    if (used * 10 >= total * 8) {
        ast_log(LOG_WARNING, "[%s] message storage %d/%d full%s\n", pvt->id, used, total, pvt->autodeletesms ? "" : " - autodeletesms is off");
    }
}

void quectel_sms_handle_urc(struct quectel_pvt* pvt, const char* line)
{
    /*
     * +CMTI: "ME",3  - a new message was stored at that index
     * +CDSI: "ME",3  - a status report was stored at that index
     *
     * Both are fetched the same way. Only queue the index here: this runs on
     * the AT reader thread, which must never issue an AT command itself - it
     * would block waiting for the very thread that parses the reply.
     */
    if (strncmp(line, "+CMTI:", 6) && strncmp(line, "+CDSI:", 6)) {
        return;
    }

    const char* const comma = strrchr(line, ',');
    if (!comma) {
        return;
    }

    if (pvt->sms_queue_count >= QUECTEL_SMS_QUEUE_MAX) {
        ast_log(LOG_WARNING, "[%s] SMS queue full, dropping index %s\n", pvt->id, comma + 1);
        return;
    }

    pvt->sms_queue[pvt->sms_queue_count++] = atoi(comma + 1);
}

void quectel_sms_process_pending(struct quectel_pvt* pvt)
{
    for (;;) {
        int index;

        ast_mutex_lock(&pvt->lock);
        if (!pvt->sms_queue_count) {
            ast_mutex_unlock(&pvt->lock);
            break;
        }
        index = pvt->sms_queue[0];
        memmove(&pvt->sms_queue[0], &pvt->sms_queue[1], (size_t)(--pvt->sms_queue_count) * sizeof(pvt->sms_queue[0]));
        ast_mutex_unlock(&pvt->lock);

        sms_fetch(pvt, index);
    }

    frag_expire(pvt);
}

/*
 * Fetch anything already sitting in the modem's storage. Messages that arrive
 * while Asterisk is down are stored but never announced with +CMTI, so without
 * this they are simply never delivered.
 *
 * Only unread messages are swept, so restarting the module does not replay
 * traffic that has already been handed to the dialplan. In PDU mode the status
 * filter is numeric: 0 = received unread.
 */
void quectel_sms_sweep_stored(struct quectel_pvt* pvt)
{
    char resp[4096];

    const int rc = quectel_at_command(pvt->at, "AT+CMGL=0", 15000, resp, sizeof(resp));
    if (rc != 0) {
        ast_log(LOG_WARNING, "[%s] cannot list stored messages (rc=%d)\n", pvt->id, rc);
        return;
    }

    /* Collect indexes first; sms_fetch() issues its own AT commands. */
    int indexes[QUECTEL_SMS_QUEUE_MAX];
    int count = 0;

    for (const char* p = resp; p && *p && count < QUECTEL_SMS_QUEUE_MAX;) {
        const char* const hit = strstr(p, "+CMGL:");
        if (!hit) {
            break;
        }
        indexes[count++] = atoi(hit + 6);
        p                = strchr(hit, '\n');
    }

    if (count) {
        ast_log(LOG_NOTICE, "[%s] %d stored message(s) to deliver\n", pvt->id, count);
    }

    for (int i = 0; i < count; i++) {
        sms_fetch(pvt, indexes[i]);
    }

    sms_check_storage(pvt);
}

/* ------------------------------------------------------------------ */
/* send                                                                */
/* ------------------------------------------------------------------ */

/*
 * Send one PDU. AT+CMGS is a two-stage command: the modem answers with "> " and
 * then consumes the hex body up to Ctrl-Z. Response tracking is armed before
 * anything is written so a fast reply cannot be missed.
 */
static int sms_send_pdu(struct quectel_pvt* pvt, const pdu_part_t* part)
{
    char hex[PDU_LENGTH * 2 + 2];
    hexify(part->buffer, part->length, hex);

    const size_t hexlen = strlen(hex);
    hex[hexlen]         = 0x1A; /* Ctrl-Z terminates the body */
    hex[hexlen + 1]     = '\0';

    char cmd[64];
    const int clen = snprintf(cmd, sizeof(cmd), "AT+CMGS=%d\r", (int)part->tpdu_length);
    if (clen < 0 || (size_t)clen >= sizeof(cmd)) {
        return -1;
    }

    if (quectel_at_arm(pvt->at) != 0) {
        return -1;
    }

    if (quectel_at_write_raw(pvt->at, cmd, (size_t)clen) != 0) {
        quectel_at_await(pvt->at, 1, NULL, 0); /* release the command lock */
        return -1;
    }

    /* Give the modem a moment to emit its "> " prompt before the body. */
    usleep(300000);

    if (quectel_at_write_raw(pvt->at, hex, hexlen + 1) != 0) {
        quectel_at_await(pvt->at, 1, NULL, 0);
        return -1;
    }

    /* Sending can take a while on a busy network. */
    return quectel_at_await(pvt->at, 60000, NULL, 0);
}

int quectel_sms_send(struct quectel_pvt* pvt, const char* number, const char* text)
{
    if (!pvt->at || pvt->state != QUECTEL_DEV_READY) {
        ast_log(LOG_WARNING, "[%s] not ready, cannot send SMS\n", pvt->id);
        return -1;
    }
    if (ast_strlen_zero(number) || ast_strlen_zero(text)) {
        return -1;
    }

    uint16_t msg16[SMS_MAX_UCS2];
    const ssize_t msg_len = utf8_to_ucs2(text, strlen(text), msg16, ARRAY_LEN(msg16));
    if (msg_len <= 0) {
        ast_log(LOG_WARNING, "[%s] message is not valid UTF-8\n", pvt->id);
        return -1;
    }

    /*
     * A UCS-2 message carries 67 characters per part, so the buffer above
     * already caps the part count at QUECTEL_SMS_MAX_PARTS. GSM7 messages pack
     * more per part and therefore need fewer.
     */
    pdu_part_t* const pdus = ast_calloc(SMS_MAX_PDUS, sizeof(*pdus));
    if (!pdus) {
        return -1;
    }

    chan_quectel_err = E_UNKNOWN;

    /* Empty SCA, not NULL: pdu_build() dereferences sca[0] unconditionally.
       An empty address makes the modem use its own configured SMSC. */
    const int parts = pdu_build_mult(pdus, "", number, msg16, (size_t)msg_len, 0 /* default validity */, 0 /* no report */, csms_ref++);
    if (parts <= 0 || parts > SMS_MAX_PDUS) {
        ast_log(LOG_WARNING, "[%s] cannot build PDU for %s: %s\n", pvt->id, number, error2str(chan_quectel_err));
        ast_free(pdus);
        return -1;
    }

    int rc = 0;
    for (int i = 0; i < parts; i++) {
        if (sms_send_pdu(pvt, &pdus[i]) != 0) {
            ast_log(LOG_WARNING, "[%s] SMS to %s failed on part %d of %d\n", pvt->id, number, i + 1, parts);
            rc = -1;
            break;
        }
    }

    ast_free(pdus);

    if (rc == 0) {
        pvt->stats.sms_out++;
        ast_log(LOG_NOTICE, "[%s] SMS sent to %s (%d part%s)\n", pvt->id, number, parts, parts == 1 ? "" : "s");
    }

    return rc;
}
