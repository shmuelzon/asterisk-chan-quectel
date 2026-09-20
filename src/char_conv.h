/*
   Copyright (C) 2020 Max von Buelow <max@m9x.de>
*/
#ifndef CHAN_QUECTEL_CHAR_CONV_H_INCLUDED
#define CHAN_QUECTEL_CHAR_CONV_H_INCLUDED

#include <stdint.h>
#include <string.h>
#include <sys/types.h> /* ssize_t size_t */

static inline uint16_t reverse_bytes_16(const uint16_t value) { return ((value & 0x00FF) << 8) | ((value & 0xFF00) >> 8); }

static inline uint32_t reverse_bytes_32(uint32_t value)
{
    return ((value & 0x000000FF) << 24) | ((value & 0x0000FF00) << 8) | ((value & 0x00FF0000) >> 8) | ((value & 0xFF000000) >> 24);
}

#if __BYTE_ORDER__ == __LITTLE_ENDIAN
static inline uint16_t little_endian_16(const uint16_t v) { return v; }

static inline uint32_t little_endian_32(const uint32_t v) { return v; }

static inline uint16_t big_endian_16(const uint16_t v) { return reverse_bytes_16(v); }

static inline uint16_t big_endian_32(const uint16_t v) { return reverse_bytes_32(v); }
#elif __BYTE_ORDER__ == __BIG_ENDIAN

static inline uint16_t little_endian_16(const uint16_t v) { return reverse_bytes_16(v); }

static inline uint16_t little_endian_32(const uint16_t v) { return reverse_bytes_32(v); }

static inline uint16_t big_endian_16(const uint16_t v) { return v; }

static inline uint32_t big_endian_32(const uint32_t v) { return v; }
#else
#error "Unsupported endianness"
#endif

ssize_t utf8_to_ucs2(const char* in, size_t in_length, uint16_t* out, size_t out_size);
ssize_t ucs2_to_utf8(const uint16_t* in, size_t in_length, char* out, size_t out_size);
int unhex(const char* in, uint8_t* out);
void hexify(const uint8_t* in, size_t in_length, char* out);
ssize_t gsm7_encode(const uint16_t* in, size_t in_length, uint16_t* out);
ssize_t gsm7_pack(const uint16_t* in, size_t in_length, char* out, size_t out_size, unsigned out_padding);
ssize_t gsm7_unpack_decode(const char* in, size_t in_length, uint16_t* out, size_t out_size, unsigned in_padding, uint8_t ls, uint8_t ss);

#endif /* CHAN_QUECTEL_CHAR_CONV_H_INCLUDED */
