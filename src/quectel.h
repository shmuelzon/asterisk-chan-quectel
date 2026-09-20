/*
 * quectel.h - shared declarations for chan_quectel
 */

#ifndef CHAN_QUECTEL_H
#define CHAN_QUECTEL_H

#include <asterisk.h>

#include <asterisk/channel.h>
#include <asterisk/linkedlists.h>
#include <asterisk/lock.h>

#include "at.h"
#include "audio.h"

#define QUECTEL_CHANNEL_TYPE "Quectel"
#define QUECTEL_CONFIG_FILE  "quectel.conf"

/* 8 kHz, 20 ms frames - the only thing the EC25-E UAC interface offers. */
#define QUECTEL_RATE          8000
#define QUECTEL_FRAME_SAMPLES 160
#define QUECTEL_FRAME_BYTES   (QUECTEL_FRAME_SAMPLES * 2)

/*
 * Reassembly slot for a concatenated (multipart) SMS. Parts can arrive out of
 * order and interleaved with other messages, so each in-flight reference gets
 * its own slot and incomplete ones are expired.
 */
#define QUECTEL_SMS_MAX_PARTS   12
#define QUECTEL_SMS_FRAG_SLOTS  4
#define QUECTEL_SMS_FRAG_TTL    600 /* seconds to wait for the remaining parts */

struct quectel_sms_frag {
    int in_use;
    unsigned char ref;
    unsigned char parts;
    int received;
    char from[64];
    char ts[64];
    char* part[QUECTEL_SMS_MAX_PARTS];
    time_t first_seen;
};

enum quectel_dev_state {
    QUECTEL_DEV_STOPPED = 0,
    QUECTEL_DEV_CONNECTING,
    QUECTEL_DEV_READY,
};

enum quectel_call_state {
    QUECTEL_CALL_IDLE = 0,
    QUECTEL_CALL_INCOMING, /* ringing at us, not answered yet */
    QUECTEL_CALL_DIALING,  /* we dialled, awaiting answer      */
    QUECTEL_CALL_ACTIVE,
};

struct quectel_stats {
    unsigned long calls_in;
    unsigned long calls_out;
    unsigned long calls_answered;
    unsigned long sms_in;
    unsigned long sms_out;
};

struct quectel_pvt {
    char id[32];
    char data_tty[128];
    char alsadev[64];
    char context[AST_MAX_CONTEXT];
    char language[MAX_LANGUAGE];
    char exten[AST_MAX_EXTENSION];
    int group;
    int rxgain; /* -1 = leave the modem alone */
    int txgain;
    int resetmodem;
    int disabled;
    int autodeletesms; /* delete a message from modem storage once delivered */

    struct quectel_at* at;
    struct quectel_audio* audio;

    ast_mutex_t lock;

    pthread_t worker;
    int worker_started;
    volatile int worker_stop;
    volatile int shutting_down;

    enum quectel_dev_state state;
    enum quectel_call_state call_state;

    struct ast_channel* owner;
    char cid_num[64];

    /* Discovered at init, shown by the CLI. */
    char model[32];
    char firmware[64];
    char imei[32];
    char imsi[32];
    char provider[64];
    char number[32];
    char smsc[32];
    int rssi;
    int registered;

    struct quectel_stats stats;

    /*
     * Indexes reported by +CMTI, to be fetched by the worker thread. The URC
     * callback runs on the AT reader thread and must never issue an AT command
     * itself - it would block waiting for the very thread that would parse the
     * reply.
     */
#define QUECTEL_SMS_QUEUE_MAX 16
    int sms_queue[QUECTEL_SMS_QUEUE_MAX];
    int sms_queue_count;

    /* Partially received concatenated messages, keyed by the UDH reference. */
    struct quectel_sms_frag frags[QUECTEL_SMS_FRAG_SLOTS];

    AST_LIST_ENTRY(quectel_pvt) entry;
};

/* Device registry, defined in chan_quectel.c */
AST_RWLIST_HEAD(quectel_devices, quectel_pvt);
extern struct quectel_devices quectel_devices;

/* Look a device up by name. Caller must hold the list lock. */
struct quectel_pvt* quectel_find_device(const char* name);

/* Pick a ready, idle device from a dial group (-1 = any). */
struct quectel_pvt* quectel_find_free_device(int group);

const char* quectel_dev_state_str(enum quectel_dev_state s);
const char* quectel_call_state_str(enum quectel_call_state s);

/* Drop any active call and release the modem side of it. */
void quectel_hangup_call(struct quectel_pvt* pvt);

int quectel_device_start(struct quectel_pvt* pvt);
void quectel_device_stop(struct quectel_pvt* pvt);

/* CLI registration, cli.c */
void quectel_cli_register(void);
void quectel_cli_unregister(void);

/* SMS, sms.c */
int quectel_sms_send(struct quectel_pvt* pvt, const char* number, const char* text);

/* Called on the AT reader thread. Only queues work - never talks to the modem. */
void quectel_sms_handle_urc(struct quectel_pvt* pvt, const char* line);

/* Called on the device worker thread; fetches anything the URC queued. */
void quectel_sms_process_pending(struct quectel_pvt* pvt);

/* Deliver messages already in modem storage (e.g. received while we were down). */
void quectel_sms_sweep_stored(struct quectel_pvt* pvt);

#endif /* CHAN_QUECTEL_H */
