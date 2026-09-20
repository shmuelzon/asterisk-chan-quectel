/*
    Copyright (C) 2020 Max von Buelow <max@m9x.de>
*/

#ifndef ERROR_H_INCLUDED
#define ERROR_H_INCLUDED

#include <asterisk/compiler.h>

enum error {
    E_UNKNOWN = 0,
    E_DEVICE_DISABLED,
    E_DEVICE_NOT_FOUND,
    E_DEVICE_DISCONNECTED,
    E_INVALID_USSD,
    E_INVALID_PHONE_NUMBER,
    E_PARSE_UTF8,
    E_PARSE_UCS2,
    E_ENCODE_GSM7,
    E_PACK_GSM7,
    E_DECODE_GSM7,
    E_SMSDB,
    E_QUEUE,
    E_BUILD_PDU,
    E_PARSE_CMGR_LINE,
    E_DEPRECATED_CMGR_TEXT,
    E_INVALID_TPDU_LENGTH,
    E_MALFORMED_HEXSTR,
    E_INVALID_SCA,
    E_INVALID_TPDU_TYPE,
    E_PARSE_TPDU,
    E_INVALID_TIMESTAMP,
    E_INVALID_CHARSET,
    E_BUILD_SCA,
    E_BUILD_PHONE_NUMBER,
    E_2BIG,
    E_CMD_FORMAT,
    E_MALLOC
};

const char* error2str(int err);

extern __thread int chan_quectel_err;

#endif
