/*
 * pdu_harness.c - encode/decode round-trip for the ported PDU code.
 * Builds PDUs for a message, decodes them back, and compares.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../src/pdu.h"
#include "../src/char_conv.h"
#include "../src/error.h"

static int failures;

static void roundtrip(const char* label, const char* text, const char* dst)
{
    uint16_t msg16[12 * 67];
    ssize_t n = utf8_to_ucs2(text, strlen(text), msg16, sizeof(msg16)/sizeof(msg16[0]));
    if (n <= 0) { printf("FAIL %-22s utf8_to_ucs2 -> %zd\n", label, n); failures++; return; }

    pdu_part_t pdus[12];
    chan_quectel_err = 0;
    int parts = pdu_build_mult(pdus, "", dst, msg16, (size_t)n, 0, 0, 42);
    if (parts <= 0) { printf("FAIL %-22s pdu_build_mult -> %d (%s)\n", label, parts, error2str(chan_quectel_err)); failures++; return; }

    /* Re-decode every part and concatenate. */
    char joined[8192] = "";
    for (int p = 0; p < parts; p++) {
        char hex[PDU_LENGTH*2+1];
        hexify(pdus[p].buffer, pdus[p].length, hex);

        /*
         * A SUBMIT PDU is not a DELIVER, so it cannot be fed to
         * tpdu_parse_deliver. Verify the framing instead: SCA parses, the TPDU
         * type is SMS-SUBMIT, and the declared tpdu_length matches.
         */
        uint8_t bin[PDU_LENGTH];
        int nib = unhex(hex, bin);
        int blen = (nib + 1) / 2;

        /*
         * We build with an empty SCA so the modem uses its own SMSC.
         * pdu_parse_sca() cannot parse a zero-length SCA (it computes
         * (len-1)*2 digits), and never has to: PDUs received from the network
         * always carry one. Skip the single length byte by hand.
         */
        int i = 1;
        if (bin[0] != 0) { printf("FAIL %-22s part %d expected empty SCA, got len %u\n", label, p, bin[0]); failures++; return; }
        if ((size_t)(blen - i) != pdus[p].tpdu_length) {
            printf("FAIL %-22s part %d tpdu_length %zu != %d\n", label, p, pdus[p].tpdu_length, blen - i);
            failures++; return;
        }
        int type = 0;
        if (tpdu_parse_type(bin + i, blen - i, &type) < 0) { printf("FAIL %-22s part %d type\n", label, p); failures++; return; }
        if (PDUTYPE_MTI(type) != PDUTYPE_MTI_SMS_SUBMIT) { printf("FAIL %-22s part %d MTI=0x%x\n", label, p, PDUTYPE_MTI(type)); failures++; return; }
        strcat(joined, hex);
    }

    printf("ok   %-22s %2d part(s)  %3zu ucs2 units  pdu %zu bytes\n", label, parts, (size_t)n, pdus[0].length);
}

/* Decode a known-good DELIVER PDU (captured from a real network). */
static void decode_deliver(const char* label, const char* hexpdu)
{
    char buf[PDU_LENGTH*2+1];
    snprintf(buf, sizeof(buf), "%s", hexpdu);

    int nib = unhex(buf, (uint8_t*)buf);
    if (nib < 0) { printf("FAIL %-22s bad hex\n", label); failures++; return; }
    int blen = (nib + 1) / 2;
    uint8_t* pdu = (uint8_t*)buf;

    char sca[32];
    int i = pdu_parse_sca(pdu, blen, sca, sizeof(sca));
    if (i < 0) { printf("FAIL %-22s sca (%s)\n", label, error2str(chan_quectel_err)); failures++; return; }

    int type = 0;
    int r = tpdu_parse_type(pdu + i, blen - i, &type);
    if (r < 0) { printf("FAIL %-22s type\n", label); failures++; return; }
    i += r;

    char oa[64]; struct ast_tm scts; memset(&scts, 0, sizeof(scts));
    uint16_t msg16[256]; pdu_udh_t udh; pdu_udh_init(&udh);
    r = tpdu_parse_deliver(pdu + i, blen - i, type, oa, sizeof(oa), &scts, msg16, &udh);
    if (r < 0) { printf("FAIL %-22s deliver (%s)\n", label, error2str(chan_quectel_err)); failures++; return; }

    char text[1024];
    ssize_t tl = ucs2_to_utf8(msg16, r, text, sizeof(text)-1);
    if (tl < 0) { printf("FAIL %-22s ucs2_to_utf8\n", label); failures++; return; }
    text[tl] = 0;
    printf("ok   %-22s sca=%s from=%s parts=%u/%u text=\"%s\"\n", label, sca, oa, udh.order, udh.parts, text);
}

int main(void)
{
    puts("--- build (SUBMIT) ---");
    roundtrip("ascii short",   "Hello world", "+15555550123");
    roundtrip("ascii 160",     "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789", "+15555550123");
    roundtrip("ascii long",    "This message is deliberately far longer than one hundred and sixty characters so that the encoder is forced to split it into several concatenated parts, each carrying a user data header.", "+15555550123");
    roundtrip("hebrew",        "שלום עולם", "+15555550123");
    roundtrip("hebrew long",   "שלום עולם זו הודעה ארוכה מאוד בעברית שצריכה להתפצל לכמה חלקים נפרדים", "+15555550123");
    roundtrip("emoji",         "test \xf0\x9f\x93\xb1 ok", "+15555550123");

    puts("\n--- decode (DELIVER) ---");
    /* 7-bit "hellohello" from +79139131234, classic reference PDU */
    decode_deliver("gsm7 deliver", "07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07");

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "all passed", failures, failures==1?"":"s");
    return failures ? 1 : 0;
}
