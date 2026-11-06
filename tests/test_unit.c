/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */
/* Unit tests for the protocol / buffering building blocks (no network needed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "cast_proto.h"
#include "json.h"
#include "log.h"
#include "mdns.h"
#include "stream.h"

static int failures;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                              \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static void test_json(void) {
  jval *v = json_parse("{\"type\":\"RECEIVER_STATUS\",\"requestId\":7,\"status\":{\"volume\":"
                       "{\"level\":0.35,\"muted\":false},\"applications\":[{\"appId\":\"CC1AD845\","
                       "\"transportId\":\"web-5\",\"sessionId\":\"ab-cd\",\"displayName\":"
                       "\"Default \\u00e9\\\"x\\\"\"}]},\"empty\":[],\"n\":null,\"neg\":-1.5e2}");
  CHECK(v != NULL);
  CHECK(!strcmp(json_str(v, "type"), "RECEIVER_STATUS"));
  CHECK(json_num(v, "requestId", 0) == 7);
  jval *st = json_get(v, "status");
  CHECK(json_num(json_get(st, "volume"), "level", 0) == 0.35);
  CHECK(json_bool(json_get(st, "volume"), "muted", 1) == 0);
  jval *app = json_get(st, "applications")->child;
  CHECK(!strcmp(json_str(app, "transportId"), "web-5"));
  CHECK(!strcmp(json_str(app, "displayName"), "Default \xc3\xa9\"x\""));
  CHECK(json_num(v, "neg", 0) == -150);
  CHECK(json_get(v, "empty")->child == NULL);
  json_free(v);
  CHECK(json_parse("{\"a\":}") == NULL);
  CHECK(json_parse("{\"a\":1} x") == NULL);
  CHECK(json_parse("[[[[") == NULL);
  char deep[200];
  memset(deep, '[', sizeof(deep) - 1);
  deep[sizeof(deep) - 1] = 0;
  CHECK(json_parse(deep) == NULL); /* depth limit, no stack overflow */
  char esc[64];
  json_escape(esc, sizeof(esc), "a\"b\\c\nd\x01");
  CHECK(!strcmp(esc, "a\\\"b\\\\c\\nd\\u0001"));
  json_escape(esc, 5, "abcdefgh");
  CHECK(strlen(esc) == 4);
}

static void test_cast_proto(void) {
  uint8_t buf[4096];
  const char *payload = "{\"type\":\"PING\"}";
  size_t n = cast_encode(buf, sizeof(buf), "sender-0", "receiver-0",
                         "urn:x-cast:com.google.cast.tp.heartbeat", payload);
  CHECK(n > 4);
  uint32_t len = (uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
  CHECK(len == n - 4);
  cast_msg m;
  CHECK(cast_decode(buf + 4, len, &m) == 0);
  CHECK(!strcmp(m.source, "sender-0"));
  CHECK(!strcmp(m.dest, "receiver-0"));
  CHECK(!strcmp(m.ns, "urn:x-cast:com.google.cast.tp.heartbeat"));
  CHECK(m.payload && !strcmp(m.payload, payload));
  cast_msg_free(&m);
  /* long payload needs a multi-byte varint length */
  char big[1000];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = 0;
  n = cast_encode(buf, sizeof(buf), "a", "b", "c", big);
  CHECK(n > 1000);
  CHECK(cast_decode(buf + 4, n - 4, &m) == 0 && m.payload_len == 999);
  cast_msg_free(&m);
  CHECK(cast_encode(buf, 100, "a", "b", "c", big) == 0); /* overflow is refused */
  /* truncated / corrupt input is rejected without reading past the end */
  CHECK(cast_decode(buf + 4, 20, &m) != 0);
  uint8_t bad[] = {0x32, 0xFF, 0xFF, 0xFF, 0x0F};
  CHECK(cast_decode(bad, sizeof(bad), &m) != 0);
}

/* builds a response packet like a Nest Audio sends */
static size_t put_name(uint8_t *p, const char *name) {
  size_t off = 0;
  while (*name) {
    const char *dot = strchr(name, '.');
    size_t l = dot ? (size_t)(dot - name) : strlen(name);
    p[off++] = (uint8_t)l;
    memcpy(p + off, name, l);
    off += l;
    name += l + (dot ? 1 : 0);
  }
  p[off++] = 0;
  return off;
}

static size_t put_rr(uint8_t *p, const uint8_t *name, size_t nl, int type, const uint8_t *rd,
                     size_t rl) {
  memcpy(p, name, nl);
  size_t o = nl;
  p[o++] = 0, p[o++] = (uint8_t)type, p[o++] = 0x80, p[o++] = 1;
  p[o++] = 0, p[o++] = 0, p[o++] = 0x11, p[o++] = 0x94;
  p[o++] = (uint8_t)(rl >> 8), p[o++] = (uint8_t)rl;
  memcpy(p + o, rd, rl);
  return o + rl;
}

static void test_mdns(void) {
  uint8_t q[256];
  size_t qn = mdns_build_query(q, sizeof(q), "_googlecast._tcp.local");
  CHECK(qn == 12 + 24 + 4);
  CHECK(q[qn - 1] == 1 && q[qn - 2] == 0x80 && q[qn - 3] == 12);

  uint8_t pkt[1500] = {0};
  pkt[2] = 0x84;
  pkt[7] = 1;  /* 1 answer */
  pkt[11] = 3; /* 3 additional */
  size_t off = 12;
  uint8_t svc[64], inst[128], host[64], rd[256];
  size_t sl = put_name(svc, "_googlecast._tcp.local");
  size_t il = put_name(inst, "Nest-Audio-abc._googlecast._tcp.local");
  size_t hl = put_name(host, "abc-def.local");
  /* PTR answer, with a compression pointer back to the service name for the rdata suffix */
  size_t ptr_name_off = off;
  memcpy(rd, inst, 15); /* label "Nest-Audio-abc" (1+14 bytes) */
  rd[15] = 0xC0, rd[16] = (uint8_t)ptr_name_off;
  off += put_rr(pkt + off, svc, sl, 12, rd, 17);
  /* TXT */
  const char *txt[] = {"id=abc123", "fn= Hallway speaker  ", "md=Nest Audio", "junk"};
  size_t tl = 0;
  for (int i = 0; i < 4; i++) {
    rd[tl++] = (uint8_t)strlen(txt[i]);
    memcpy(rd + tl, txt[i], strlen(txt[i]));
    tl += strlen(txt[i]);
  }
  off += put_rr(pkt + off, inst, il, 16, rd, tl);
  /* SRV */
  memset(rd, 0, 6);
  rd[4] = 0x1F, rd[5] = 0x49; /* 8009 */
  memcpy(rd + 6, host, hl);
  off += put_rr(pkt + off, inst, il, 33, rd, 6 + hl);
  /* A */
  uint8_t ip[4] = {192, 168, 1, 81};
  off += put_rr(pkt + off, host, hl, 1, ip, 4);

  static mdns_acc acc;
  memset(&acc, 0, sizeof(acc));
  CHECK(mdns_parse_packet(pkt, off, &acc) == 0);
  cast_device out[4];
  int n = mdns_collect(&acc, out, 4);
  CHECK(n == 1);
  if (n == 1) {
    CHECK(!strcmp(out[0].instance, "Nest-Audio-abc"));
    CHECK(!strcmp(out[0].name, "Hallway speaker"));
    CHECK(!strcmp(out[0].id, "abc123"));
    CHECK(!strcmp(out[0].model, "Nest Audio"));
    CHECK(!strcmp(out[0].ip, "192.168.1.81"));
    CHECK(out[0].port == 8009);
  }
  /* every truncation of the packet must be handled safely */
  for (size_t cut = 0; cut < off; cut++) {
    memset(&acc, 0, sizeof(acc));
    mdns_parse_packet(pkt, cut, &acc);
  }
  /* compression loop */
  uint8_t loop[] = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xC0, 12};
  memset(&acc, 0, sizeof(acc));
  CHECK(mdns_parse_packet(loop, sizeof(loop), &acc) != 0);
}

static void test_stream(void) {
  stream s;
  CHECK(stream_init(&s, "dev", 48000, 2, 2000, 1000) == 0);
  CHECK(!strcmp(s.path, "/stream/dev.wav"));
  CHECK(s.cap == 48000 * 2 * 4);
  uint8_t pcm[4000];
  for (size_t i = 0; i < sizeof(pcm); i++)
    pcm[i] = (uint8_t)i;
  /* write 3 s into a 2 s ring */
  for (int i = 0; i < 144; i++)
    stream_write(&s, pcm, sizeof(pcm));
  CHECK(s.wpos == 144 * 4000);
  uint64_t start = stream_start_pos(&s);
  CHECK(start % 4 == 0);
  CHECK(s.wpos - start <= 192000 + 4); /* 1 s backlog cap */
  CHECK(s.wpos - start >= 192000 - 4);
  /* data at the ring position matches what was written */
  size_t at = (size_t)((s.wpos - 4000) % s.cap);
  CHECK(at + 4000 > s.cap || !memcmp(s.buf + at, pcm, 4000));
  /* session start is respected when inside the backlog window */
  stream_mark_session_next(&s);
  stream_write(&s, pcm, sizeof(pcm));
  CHECK(stream_start_pos(&s) == s.wpos - 4000);
  stream_destroy(&s);

  uint8_t h[44];
  wav_header(h, 48000, 2);
  CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
  CHECK(h[24] == 0x80 && h[25] == 0xBB); /* 48000 */
  CHECK(h[32] == 4 && h[34] == 16);
}

static void test_ids(void) {
  uint64_t a = airplay_device_id_for("f18510440ea81251a023e6a9e4984106");
  uint64_t b = airplay_device_id_for("4e147c2405768e20009df87d79e24cab");
  CHECK(a != b);
  CHECK(a == airplay_device_id_for("f18510440ea81251a023e6a9e4984106"));
  CHECK((a >> 48) == 0);
  CHECK(((a >> 40) & 0x03) == 0x02); /* locally administered unicast */
}

int main(void) {
  log_level = L_WARN;
  test_json();
  test_cast_proto();
  test_mdns();
  test_stream();
  test_ids();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  puts("all unit tests passed");
  return 0;
}
