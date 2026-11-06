/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */

#ifndef AP2C_CAST_PROTO_H
#define AP2C_CAST_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Google Cast v2 "CastMessage" protobuf (cast_channel.proto), string payloads only.
 *   1 protocol_version (enum, CASTV2_1_0 = 0)
 *   2 source_id        (string)
 *   3 destination_id   (string)
 *   4 namespace        (string)
 *   5 payload_type     (enum, STRING = 0, BINARY = 1)
 *   6 payload_utf8     (string)
 *   7 payload_binary   (bytes)
 * On the wire each message is prefixed by a 4-byte big-endian length. */

#define CAST_MAX_MSG (64 * 1024)

typedef struct {
  char source[128];
  char dest[128];
  char ns[128];
  int payload_type;
  char *payload; /* NUL-terminated, malloc'd; NULL for binary payloads */
  size_t payload_len;
} cast_msg;

/* Encodes a framed message (length prefix included). Returns total bytes, or 0 if it doesn't fit. */
size_t cast_encode(uint8_t *buf, size_t cap, const char *src, const char *dst, const char *ns,
                   const char *payload);

/* Decodes a message body (without the length prefix). Returns 0 on success. Free with cast_msg_free. */
int cast_decode(const uint8_t *buf, size_t len, cast_msg *m);
void cast_msg_free(cast_msg *m);

#endif
