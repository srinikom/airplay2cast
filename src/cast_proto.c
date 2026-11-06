/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */

#include "cast_proto.h"

#include <stdlib.h>
#include <string.h>

static size_t put_varint(uint8_t *p, uint64_t v) {
  size_t n = 0;
  do {
    uint8_t b = v & 0x7F;
    v >>= 7;
    p[n++] = b | (v ? 0x80 : 0);
  } while (v);
  return n;
}

static int get_varint(const uint8_t *p, size_t len, size_t *off, uint64_t *out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (*off >= len)
      return -1;
    uint8_t b = p[(*off)++];
    v |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      *out = v;
      return 0;
    }
  }
  return -1;
}

/* Appends a length-delimited field; returns new offset or 0 on overflow. */
static size_t put_bytes(uint8_t *buf, size_t cap, size_t off, int field, const char *s) {
  size_t sl = strlen(s);
  if (off + 1 + 10 + sl > cap)
    return 0;
  buf[off++] = (uint8_t)((field << 3) | 2);
  off += put_varint(buf + off, sl);
  memcpy(buf + off, s, sl);
  return off + sl;
}

size_t cast_encode(uint8_t *buf, size_t cap, const char *src, const char *dst, const char *ns,
                   const char *payload) {
  size_t off = 4;
  if (cap < 8)
    return 0;
  buf[off++] = (1 << 3) | 0; /* protocol_version = CASTV2_1_0 */
  buf[off++] = 0;
  if (!(off = put_bytes(buf, cap, off, 2, src)) || !(off = put_bytes(buf, cap, off, 3, dst)) ||
      !(off = put_bytes(buf, cap, off, 4, ns)))
    return 0;
  if (off + 2 > cap)
    return 0;
  buf[off++] = (5 << 3) | 0; /* payload_type = STRING */
  buf[off++] = 0;
  if (!(off = put_bytes(buf, cap, off, 6, payload)))
    return 0;
  uint32_t body = (uint32_t)(off - 4);
  buf[0] = (uint8_t)(body >> 24);
  buf[1] = (uint8_t)(body >> 16);
  buf[2] = (uint8_t)(body >> 8);
  buf[3] = (uint8_t)body;
  return off;
}

static void copy_field(char *dst, size_t cap, const uint8_t *src, size_t n) {
  if (n >= cap)
    n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

int cast_decode(const uint8_t *buf, size_t len, cast_msg *m) {
  memset(m, 0, sizeof(*m));
  size_t off = 0;
  while (off < len) {
    uint64_t key, v;
    if (get_varint(buf, len, &off, &key) < 0)
      goto fail;
    int field = (int)(key >> 3), wt = (int)(key & 7);
    if (wt == 0) {
      if (get_varint(buf, len, &off, &v) < 0)
        goto fail;
      if (field == 5)
        m->payload_type = (int)v;
    } else if (wt == 2) {
      if (get_varint(buf, len, &off, &v) < 0 || v > len - off)
        goto fail;
      const uint8_t *d = buf + off;
      switch (field) {
      case 2: copy_field(m->source, sizeof(m->source), d, v); break;
      case 3: copy_field(m->dest, sizeof(m->dest), d, v); break;
      case 4: copy_field(m->ns, sizeof(m->ns), d, v); break;
      case 6:
        free(m->payload);
        m->payload = malloc(v + 1);
        if (!m->payload)
          goto fail;
        memcpy(m->payload, d, v);
        m->payload[v] = 0;
        m->payload_len = v;
        break;
      default: break; /* binary payload and unknown fields are ignored */
      }
      off += v;
    } else if (wt == 5) {
      off += 4;
    } else if (wt == 1) {
      off += 8;
    } else {
      goto fail;
    }
  }
  if (off != len)
    goto fail;
  return 0;
fail:
  cast_msg_free(m);
  return -1;
}

void cast_msg_free(cast_msg *m) {
  free(m->payload);
  m->payload = NULL;
}
