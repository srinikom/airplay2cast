/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */

#include "mdns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

#define SERVICE "_googlecast._tcp.local"
#define T_A 1
#define T_PTR 12
#define T_TXT 16
#define T_SRV 33

size_t mdns_build_query(uint8_t *buf, size_t cap, const char *service) {
  size_t off = 12;
  if (cap < 12 + strlen(service) + 6)
    return 0;
  memset(buf, 0, 12);
  buf[5] = 1; /* QDCOUNT = 1; ID = 0, flags = 0 */
  const char *p = service;
  while (*p) {
    const char *dot = strchr(p, '.');
    size_t l = dot ? (size_t)(dot - p) : strlen(p);
    if (l == 0 || l > 63)
      return 0;
    buf[off++] = (uint8_t)l;
    memcpy(buf + off, p, l);
    off += l;
    p += l;
    if (*p == '.')
      p++;
  }
  buf[off++] = 0;
  buf[off++] = 0, buf[off++] = T_PTR;
  buf[off++] = 0x80, buf[off++] = 0x01; /* QU bit + class IN */
  return off;
}

/* Decodes a (possibly compressed) name at *off into out (dotted). Returns 0 on success. */
static int read_name(const uint8_t *pkt, size_t len, size_t *off, char *out, size_t cap) {
  size_t pos = *off, n = 0;
  int jumped = 0, hops = 0;
  out[0] = 0;
  for (;;) {
    if (pos >= len)
      return -1;
    uint8_t l = pkt[pos];
    if ((l & 0xC0) == 0xC0) {
      if (pos + 1 >= len || ++hops > 16)
        return -1;
      size_t ptr = ((size_t)(l & 0x3F) << 8) | pkt[pos + 1];
      if (!jumped)
        *off = pos + 2;
      jumped = 1;
      pos = ptr;
      continue;
    }
    if (l & 0xC0)
      return -1;
    pos++;
    if (l == 0)
      break;
    if (pos + l > len)
      return -1;
    if (n && n + 1 < cap)
      out[n++] = '.';
    for (uint8_t i = 0; i < l && n + 1 < cap; i++)
      out[n++] = (char)pkt[pos + i];
    out[n] = 0;
    pos += l;
  }
  if (!jumped)
    *off = pos;
  return 0;
}

static int ends_with_service(const char *name) {
  size_t nl = strlen(name), sl = strlen(SERVICE);
  return nl > sl + 1 && !strcasecmp(name + nl - sl, SERVICE) && name[nl - sl - 1] == '.';
}

static cast_device *get_dev(mdns_acc *acc, const char *fullname) {
  char inst[128];
  size_t l = strlen(fullname) - strlen(SERVICE) - 1;
  if (l >= sizeof(inst))
    l = sizeof(inst) - 1;
  memcpy(inst, fullname, l);
  inst[l] = 0;
  for (int i = 0; i < acc->ndev; i++)
    if (!strcmp(acc->dev[i].instance, inst))
      return &acc->dev[i];
  if (acc->ndev >= MDNS_MAX_DEVICES)
    return NULL;
  cast_device *d = &acc->dev[acc->ndev++];
  memset(d, 0, sizeof(*d));
  snprintf(d->instance, sizeof(d->instance), "%s", inst);
  return d;
}

static void parse_txt(cast_device *d, const uint8_t *p, size_t len) {
  size_t off = 0;
  while (off < len) {
    size_t l = p[off++];
    if (off + l > len)
      break;
    char kv[256];
    size_t cl = l < sizeof(kv) - 1 ? l : sizeof(kv) - 1;
    memcpy(kv, p + off, cl);
    kv[cl] = 0;
    off += l;
    char *eq = strchr(kv, '=');
    if (!eq)
      continue;
    *eq = 0;
    const char *v = eq + 1;
    if (!strcmp(kv, "id"))
      snprintf(d->id, sizeof(d->id), "%s", v);
    else if (!strcmp(kv, "fn")) {
      while (*v == ' ')
        v++;
      snprintf(d->name, sizeof(d->name), "%s", v);
      for (size_t n = strlen(d->name); n && d->name[n - 1] == ' ';)
        d->name[--n] = 0;
    }
    else if (!strcmp(kv, "md"))
      snprintf(d->model, sizeof(d->model), "%s", v);
  }
}

int mdns_parse_packet(const uint8_t *pkt, size_t len, mdns_acc *acc) {
  if (len < 12)
    return -1;
  if (!(pkt[2] & 0x80)) /* not a response */
    return 0;
  unsigned qd = (unsigned)(pkt[4] << 8 | pkt[5]);
  unsigned rr = (unsigned)(pkt[6] << 8 | pkt[7]) + (unsigned)(pkt[8] << 8 | pkt[9]) +
                (unsigned)(pkt[10] << 8 | pkt[11]);
  size_t off = 12;
  char name[256], rname[256];
  for (unsigned i = 0; i < qd; i++) {
    if (read_name(pkt, len, &off, name, sizeof(name)) < 0 || off + 4 > len)
      return -1;
    off += 4;
  }
  for (unsigned i = 0; i < rr; i++) {
    if (read_name(pkt, len, &off, name, sizeof(name)) < 0 || off + 10 > len)
      return -1;
    unsigned type = (unsigned)(pkt[off] << 8 | pkt[off + 1]);
    size_t rdlen = (size_t)(pkt[off + 8] << 8 | pkt[off + 9]);
    off += 10;
    if (off + rdlen > len)
      return -1;
    const uint8_t *rd = pkt + off;
    size_t rdoff = off;
    off += rdlen;
    cast_device *d;
    switch (type) {
    case T_PTR:
      if (strcasecmp(name, SERVICE))
        break;
      if (read_name(pkt, len, &rdoff, rname, sizeof(rname)) == 0 && ends_with_service(rname))
        get_dev(acc, rname);
      break;
    case T_SRV:
      if (!ends_with_service(name) || rdlen < 7 || !(d = get_dev(acc, name)))
        break;
      d->port = rd[4] << 8 | rd[5];
      rdoff += 6;
      read_name(pkt, len, &rdoff, d->target, sizeof(d->target));
      break;
    case T_TXT:
      if (ends_with_service(name) && (d = get_dev(acc, name)))
        parse_txt(d, rd, rdlen);
      break;
    case T_A:
      if (rdlen == 4 && acc->naddr < (int)(sizeof(acc->addr) / sizeof(acc->addr[0]))) {
        int k;
        for (k = 0; k < acc->naddr; k++)
          if (!strcasecmp(acc->addr[k].host, name))
            break;
        if (k == acc->naddr)
          acc->naddr++;
        snprintf(acc->addr[k].host, sizeof(acc->addr[k].host), "%s", name);
        inet_ntop(AF_INET, rd, acc->addr[k].ip, sizeof(acc->addr[k].ip));
      }
      break;
    default: break;
    }
  }
  return 0;
}

int mdns_collect(mdns_acc *acc, cast_device *out, int max) {
  int n = 0;
  for (int i = 0; i < acc->ndev && n < max; i++) {
    cast_device *d = &acc->dev[i];
    if (!d->ip[0])
      for (int k = 0; k < acc->naddr; k++)
        if (!strcasecmp(acc->addr[k].host, d->target))
          snprintf(d->ip, sizeof(d->ip), "%s", acc->addr[k].ip);
    if (d->ip[0] && d->port && d->id[0] && d->name[0])
      out[n++] = *d;
  }
  return n;
}

static int send_query(int fd, const char *name, uint16_t type) {
  uint8_t q[512];
  size_t n = mdns_build_query(q, sizeof(q), name);
  if (!n)
    return -1;
  q[n - 3] = (uint8_t)type;
  struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(5353)};
  inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
  return sendto(fd, q, n, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0 ? -1 : 0;
}

int mdns_browse_cast(cast_device *out, int max, int timeout_ms, const char *ifaddr) {
  static mdns_acc acc; /* large; browse is only called from the main thread */
  memset(&acc, 0, sizeof(acc));
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  unsigned char ttl = 255;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  if (ifaddr && *ifaddr) {
    struct in_addr ia;
    if (inet_pton(AF_INET, ifaddr, &ia) == 1)
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof(ia));
  }
  int64_t start = now_ms(), deadline = start + timeout_ms;
  int sends = 0;
  int64_t next_send = start;
  uint8_t pkt[9000];
  for (;;) {
    int64_t t = now_ms();
    if (t >= deadline)
      break;
    if (sends < 3 && t >= next_send) {
      if (send_query(fd, SERVICE, T_PTR) < 0)
        LOGW("mdns: query failed: %s", strerror(errno));
      sends++;
      next_send = t + 700;
    }
    int wait = (int)((sends < 3 ? next_send : deadline) - t);
    if (wait > (int)(deadline - t))
      wait = (int)(deadline - t);
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, wait > 0 ? wait : 0) <= 0)
      continue;
    ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
    if (n > 0)
      mdns_parse_packet(pkt, (size_t)n, &acc);
  }
  /* resolve any SRV targets we have no address for */
  int missing = 0;
  for (int i = 0; i < acc.ndev; i++) {
    int found = 0;
    for (int k = 0; k < acc.naddr; k++)
      if (!strcasecmp(acc.addr[k].host, acc.dev[i].target))
        found = 1;
    if (acc.dev[i].target[0] && !found) {
      send_query(fd, acc.dev[i].target, T_A);
      missing = 1;
    }
  }
  if (missing) {
    int64_t end = now_ms() + 700;
    for (int64_t t; (t = now_ms()) < end;) {
      struct pollfd p = {.fd = fd, .events = POLLIN};
      if (poll(&p, 1, (int)(end - t)) <= 0)
        continue;
      ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
      if (n > 0)
        mdns_parse_packet(pkt, (size_t)n, &acc);
    }
  }
  close(fd);
  return mdns_collect(&acc, out, max);
}
