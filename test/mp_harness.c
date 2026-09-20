/* mp_harness.c - decode concatenated DELIVER PDUs and reassemble, offline. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../src/pdu.h"
#include "../src/char_conv.h"
#include "../src/error.h"

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <file-of-hex-pdus>\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "r");
    if (!f) { perror("open"); return 2; }

    char joined[8192] = "";
    char line[4096];
    int n = 0;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;

        int nib = unhex(line, (uint8_t*)line);
        if (nib < 0) { printf("part %d: bad hex\n", ++n); continue; }
        int blen = (nib + 1) / 2;
        uint8_t* pdu = (uint8_t*)line;

        char sca[32];
        int i = pdu_parse_sca(pdu, blen, sca, sizeof(sca));
        if (i < 0) { printf("part %d: sca failed (%s)\n", ++n, error2str(chan_quectel_err)); continue; }

        int type = 0;
        int r = tpdu_parse_type(pdu + i, blen - i, &type);
        if (r < 0) { printf("part %d: type failed\n", ++n); continue; }
        i += r;

        char oa[64]; struct ast_tm scts; memset(&scts, 0, sizeof scts);
        uint16_t msg16[512]; pdu_udh_t udh; pdu_udh_init(&udh);
        r = tpdu_parse_deliver(pdu + i, blen - i, type, oa, sizeof oa, &scts, msg16, &udh);
        if (r < 0) { printf("part %d: deliver failed (%s)\n", ++n, error2str(chan_quectel_err)); continue; }

        char text[4096];
        ssize_t tl = ucs2_to_utf8(msg16, r, text, sizeof(text) - 1);
        if (tl < 0) { printf("part %d: ucs2_to_utf8 failed\n", ++n); continue; }
        text[tl] = 0;

        n++;
        printf("part %d/%u ref=%u units=%d utf8=%zd  first12=[%.24s]\n", udh.order, udh.parts, udh.ref, r, tl, text);
        strncat(joined, text, sizeof(joined) - strlen(joined) - 1);
    }
    fclose(f);

    printf("\nreassembled (%zu bytes):\n%s\n", strlen(joined), joined);

    /* flag any invalid UTF-8 in the result */
    int bad = 0;
    for (const unsigned char* p = (const unsigned char*)joined; *p;) {
        int len = (*p < 0x80) ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : -1;
        if (len < 0) { bad++; p++; continue; }
        for (int k = 1; k < len; k++) if ((p[k] & 0xC0) != 0x80) { bad++; break; }
        p += len;
    }
    printf("\ninvalid UTF-8 sequences: %d  -> %s\n", bad, bad ? "CORRUPT" : "clean");
    return bad ? 1 : 0;
}
