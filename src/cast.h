/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */

#ifndef AP2C_CAST_H
#define AP2C_CAST_H

#include <stddef.h>
#include <stdint.h>

#include "cast_proto.h"

#define CAST_APP_DEFAULT_MEDIA_RECEIVER "CC1AD845"

struct gnutls_session_int; /* gnutls_session_t */

typedef struct cast_conn {
  int fd;
  struct gnutls_session_int *tls;
  char tag[96];  /* log prefix */
  char host[64];
  int port;

  uint8_t rbuf[CAST_MAX_MSG + 4];
  size_t rlen;
  int next_req;
  int64_t last_rx_ms, last_ping_ms;

  /* receiver state (from RECEIVER_STATUS) */
  int have_volume;
  double volume;
  int muted;
  char app_id[32];
  int idle_screen;        /* app_id is the speaker's idle/backdrop screen */
  char transport_id[64];
  char session_id[64];
  int app_connected; /* virtual connection to transport_id opened */
  int closed_by_receiver; /* our app connection was closed by the speaker; cleared by caller */

  /* media state (from MEDIA_STATUS) */
  long media_session_id;
  long loaded_session_id; /* media session created by our last LOAD */
  double current_time;
  char player_state[24];
  char idle_reason[32];
  int media_status_seq; /* incremented on each MEDIA_STATUS */
  int volume_seq;       /* incremented on each RECEIVER_STATUS carrying volume */

  /* pending request tracking */
  int wait_req;
  int got_resp;
  char resp_type[32];
} cast_conn;

void cast_global_init(void);

/* Connects (TLS) and opens the platform virtual connection. Returns 0 on success. */
int cast_open(cast_conn *c, const char *tag, const char *host, int port, int timeout_ms);
void cast_close(cast_conn *c);
static inline int cast_is_open(const cast_conn *c) { return c->tls != 0; }
int cast_fd(const cast_conn *c);

/* Reads and processes incoming messages for up to timeout_ms, sends heartbeats.
 * Returns 0 normally, -1 if the connection failed (it is closed in that case). */
int cast_pump(cast_conn *c, int timeout_ms);

int cast_get_status(cast_conn *c, int timeout_ms);
int cast_launch(cast_conn *c, const char *app_id, int timeout_ms);
int cast_load(cast_conn *c, const char *url, const char *content_type, const char *title,
              const char *artist, int timeout_ms);
int cast_media_command(cast_conn *c, const char *type, int timeout_ms); /* PLAY/PAUSE/STOP */
int cast_set_volume(cast_conn *c, double level, int timeout_ms);
int cast_stop_app(cast_conn *c, int timeout_ms);

#endif
