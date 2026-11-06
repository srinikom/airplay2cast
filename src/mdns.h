/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */

#ifndef AP2C_MDNS_H
#define AP2C_MDNS_H

#include <stddef.h>
#include <stdint.h>

/* Minimal mDNS browser for Google Cast devices (_googlecast._tcp.local). Queries are
 * sent as RFC 6762 legacy-unicast queries from an ephemeral port, so it does not need
 * port 5353 and coexists with avahi-daemon / systemd-resolved. */

#define MDNS_MAX_DEVICES 32

typedef struct {
  char instance[128]; /* first label of the service instance name */
  char id[64];        /* TXT id= (stable device / group UUID) */
  char name[128];     /* TXT fn= (friendly name) */
  char model[64];     /* TXT md= */
  char target[128];   /* SRV target host */
  char ip[64];        /* IPv4 address */
  int port;           /* SRV port */
} cast_device;

typedef struct {
  cast_device dev[MDNS_MAX_DEVICES];
  int ndev;
  struct {
    char host[128];
    char ip[64];
  } addr[MDNS_MAX_DEVICES * 2];
  int naddr;
} mdns_acc;

/* Browses for `timeout_ms`. `ifaddr` (optional) selects the multicast interface by IPv4
 * address. Returns the number of complete devices written to out. */
int mdns_browse_cast(cast_device *out, int max, int timeout_ms, const char *ifaddr);

/* Exposed for tests: accumulate one response packet / extract complete devices. */
int mdns_parse_packet(const uint8_t *pkt, size_t len, mdns_acc *acc);
int mdns_collect(mdns_acc *acc, cast_device *out, int max);
size_t mdns_build_query(uint8_t *buf, size_t cap, const char *service);

#endif
