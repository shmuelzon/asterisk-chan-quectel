/*
    error.c
*/
#include "error.h"

#include "mutils.h"

const char* error2str(int err)
{
    static const char* const errors[] = {
        "Unknown error",
        "Device disbaled",
        "Device not found",
        "Device disconnected",
        "Invalid USSD",
        "Invalid phone number",
        "Cannot parse UTF-8",
        "Cannot parse UCS-2",
        "Cannot encode GSM7",
        "Cannot pack GSM7",
        "Cannot decode GSM7",
        "SMSDB error",
        "Queue error",
        "PDU building error",
        "Can't parse +CMGR response line",
        "Parsing messages in TEXT mode is not supported anymore; This message should never appear. Nevertheless, if "
        "this message appears, please report on GitHub.",
        "Invalid TPDU length in CMGR PDU status line",
        "Malformed hex string",
        "Invalid SCA",
        "Invalid TPDU type",
        "Cannot parse TPDU",
        "Invalid timestamp",
        "Invalid charset",
        "Cannot build SCA",
        "Cannot build phone number",
        "Input too large",
        "Fail to format AT command",
        "Unable to allocate memory",
    };
    return enum2str(err, errors, ARRAY_LEN(errors));
}

__thread int chan_quectel_err;
