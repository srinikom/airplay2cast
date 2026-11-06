#include "cast.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "json.h"
#include "log.h"

#define NS_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA "urn:x-cast:com.google.cast.media"
#define SENDER_ID "sender-ap2c"
#define RECEIVER_ID "receiver-0"

#define PING_INTERVAL_MS 5000
#define DEAD_TIMEOUT_MS 20000

static SSL_CTX *g_ctx;

void cast_global_init(void) {
  if (g_ctx)
    return;
  OPENSSL_init_ssl(0, NULL);
  g_ctx = SSL_CTX_new(TLS_client_method());
  /* Cast devices present self-signed device certificates; the Cast device-auth
   * challenge is not required by the Default Media Receiver. */
  SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
  SSL_CTX_set_mode(g_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);
}

int cast_fd(const cast_conn *c) { return c->ssl ? c->fd : -1; }

static int wait_fd(int fd, short ev, int timeout_ms) {
  struct pollfd p = {.fd = fd, .events = ev};
  int r;
  do
    r = poll(&p, 1, timeout_ms);
  while (r < 0 && errno == EINTR);
  return r;
}

static void teardown(cast_conn *c) {
  if (c->ssl) {
    SSL_free(c->ssl);
    c->ssl = NULL;
  }
  if (c->fd >= 0)
    close(c->fd);
  c->fd = -1;
  c->rlen = 0;
  c->app_id[0] = c->transport_id[0] = c->session_id[0] = 0;
  c->app_connected = 0;
  c->media_session_id = 0;
  c->player_state[0] = 0;
}

static int ssl_write_all(cast_conn *c, const uint8_t *buf, size_t len) {
  size_t off = 0;
  int64_t deadline = now_ms() + 5000;
  while (off < len) {
    int n = SSL_write(c->ssl, buf + off, (int)(len - off));
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    int err = SSL_get_error(c->ssl, n);
    int left = (int)(deadline - now_ms());
    if (left <= 0)
      return -1;
    if (err == SSL_ERROR_WANT_WRITE)
      wait_fd(c->fd, POLLOUT, left);
    else if (err == SSL_ERROR_WANT_READ)
      wait_fd(c->fd, POLLIN, left);
    else
      return -1;
  }
  return 0;
}

static int send_raw(cast_conn *c, const char *dst, const char *ns, const char *payload) {
  static __thread uint8_t buf[CAST_MAX_MSG];
  if (!c->ssl)
    return -1;
  size_t n = cast_encode(buf, sizeof(buf), SENDER_ID, dst, ns, payload);
  if (!n) {
    LOGE("%s: message too large", c->tag);
    return -1;
  }
  if (strcmp(ns, NS_HEARTBEAT))
    LOGD("%s: -> %s %s", c->tag, dst, payload);
  if (ssl_write_all(c, buf, n) < 0) {
    LOGW("%s: write failed", c->tag);
    return -1;
  }
  return 0;
}

/* Sends a request; `json` must start with '{' and will get a requestId inserted. */
static int send_req(cast_conn *c, const char *dst, const char *ns, const char *json) {
  char buf[8192];
  int id = ++c->next_req;
  if (snprintf(buf, sizeof(buf), "{\"requestId\":%d,%s", id, json + 1) >= (int)sizeof(buf))
    return -1;
  c->wait_req = id;
  c->got_resp = 0;
  c->resp_type[0] = 0;
  return send_raw(c, dst, ns, buf) < 0 ? -1 : id;
}

static void handle_receiver_status(cast_conn *c, jval *st) {
  jval *vol = json_get(st, "volume");
  if (vol) {
    c->have_volume = 1;
    c->volume = json_num(vol, "level", c->volume);
    c->muted = json_bool(vol, "muted", c->muted);
    c->volume_seq++;
  }
  jval *apps = json_get(st, "applications");
  if (!apps) /* volume-only status updates omit applications */
    return;
  jval *app = (apps->type == J_ARR) ? apps->child : NULL;
  const char *tid = app ? json_str(app, "transportId") : NULL;
  const char *aid = app ? json_str(app, "appId") : NULL;
  const char *sid = app ? json_str(app, "sessionId") : NULL;
  if (!tid || strcmp(tid, c->transport_id)) {
    c->app_connected = 0;
    c->media_session_id = 0;
    c->player_state[0] = 0;
  }
  snprintf(c->transport_id, sizeof(c->transport_id), "%s", tid ? tid : "");
  snprintf(c->app_id, sizeof(c->app_id), "%s", aid ? aid : "");
  snprintf(c->session_id, sizeof(c->session_id), "%s", sid ? sid : "");
}

static void handle_media_status(cast_conn *c, jval *root) {
  jval *arr = json_get(root, "status");
  jval *st = (arr && arr->type == J_ARR) ? arr->child : NULL;
  c->media_status_seq++;
  if (!st) {
    c->media_session_id = 0;
    c->player_state[0] = 0;
    return;
  }
  long msid = (long)json_num(st, "mediaSessionId", (double)c->media_session_id);
  if (c->loaded_session_id && msid < c->loaded_session_id)
    return; /* late status for a session we already replaced */
  c->media_session_id = msid;
  c->current_time = json_num(st, "currentTime", c->current_time);
  const char *ps = json_str(st, "playerState");
  const char *ir = json_str(st, "idleReason");
  snprintf(c->player_state, sizeof(c->player_state), "%s", ps ? ps : "");
  snprintf(c->idle_reason, sizeof(c->idle_reason), "%s", ir ? ir : "");
  if (ps && !strcmp(ps, "IDLE") && ir)
    LOGI("%s: media idle (%s)", c->tag, ir);
}

static void handle_message(cast_conn *c, cast_msg *m) {
  if (!m->payload)
    return;
  jval *root = json_parse(m->payload);
  if (!root) {
    LOGD("%s: unparsable payload on %s", c->tag, m->ns);
    return;
  }
  const char *type = json_str(root, "type");
  if (!type)
    type = "";
  if (!strcmp(m->ns, NS_HEARTBEAT)) {
    if (!strcmp(type, "PING"))
      send_raw(c, m->source, NS_HEARTBEAT, "{\"type\":\"PONG\"}");
    json_free(root);
    return;
  }
  LOGD("%s: <- %s %s", c->tag, m->source, m->payload);
  if (!strcmp(m->ns, NS_CONNECTION) && !strcmp(type, "CLOSE")) {
    if (!strcmp(m->source, c->transport_id)) {
      LOGI("%s: receiver application closed our connection", c->tag);
      c->app_connected = 0;
      c->media_session_id = 0;
      c->player_state[0] = 0;
      c->transport_id[0] = 0;
    }
  } else if (!strcmp(m->ns, NS_RECEIVER)) {
    jval *st = json_get(root, "status");
    if (!strcmp(type, "RECEIVER_STATUS") && st)
      handle_receiver_status(c, st);
  } else if (!strcmp(m->ns, NS_MEDIA)) {
    if (!strcmp(type, "MEDIA_STATUS"))
      handle_media_status(c, root);
    else if (strstr(type, "FAILED") || strstr(type, "INVALID") || strstr(type, "CANCELLED"))
      LOGW("%s: media error %s: %s", c->tag, type, m->payload);
  }
  int rid = (int)json_num(root, "requestId", 0);
  if (rid && rid == c->wait_req) {
    c->got_resp = 1;
    snprintf(c->resp_type, sizeof(c->resp_type), "%s", type);
  }
  json_free(root);
}

/* Drains whatever is readable right now. Returns -1 on connection error. */
static int read_available(cast_conn *c) {
  for (;;) {
    if (c->rlen >= sizeof(c->rbuf))
      return -1;
    int n = SSL_read(c->ssl, c->rbuf + c->rlen, (int)(sizeof(c->rbuf) - c->rlen));
    if (n <= 0) {
      int err = SSL_get_error(c->ssl, n);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        break;
      return -1;
    }
    c->rlen += (size_t)n;
    c->last_rx_ms = now_ms();
    /* process complete frames */
    size_t off = 0;
    while (c->rlen - off >= 4) {
      uint32_t len = ((uint32_t)c->rbuf[off] << 24) | ((uint32_t)c->rbuf[off + 1] << 16) |
                     ((uint32_t)c->rbuf[off + 2] << 8) | c->rbuf[off + 3];
      if (len > CAST_MAX_MSG)
        return -1;
      if (c->rlen - off - 4 < len)
        break;
      cast_msg m;
      if (cast_decode(c->rbuf + off + 4, len, &m) == 0) {
        handle_message(c, &m);
        cast_msg_free(&m);
      }
      off += 4 + len;
      if (!c->ssl)
        return -1;
    }
    if (off) {
      memmove(c->rbuf, c->rbuf + off, c->rlen - off);
      c->rlen -= off;
    }
  }
  return 0;
}

int cast_pump(cast_conn *c, int timeout_ms) {
  if (!c->ssl)
    return -1;
  int64_t t = now_ms();
  if (t - c->last_ping_ms >= PING_INTERVAL_MS) {
    c->last_ping_ms = t;
    if (send_raw(c, RECEIVER_ID, NS_HEARTBEAT, "{\"type\":\"PING\"}") < 0)
      goto dead;
  }
  if (t - c->last_rx_ms > DEAD_TIMEOUT_MS) {
    LOGW("%s: no traffic for %d s, connection lost", c->tag, DEAD_TIMEOUT_MS / 1000);
    goto dead;
  }
  if (!SSL_pending(c->ssl) && timeout_ms > 0) {
    int r = wait_fd(c->fd, POLLIN, timeout_ms);
    if (r <= 0)
      return 0;
  }
  if (read_available(c) < 0)
    goto dead;
  return 0;
dead:
  LOGW("%s: connection closed", c->tag);
  teardown(c);
  return -1;
}

/* Pumps until the response to request `id` arrives. Returns 0, or -1 on timeout/error. */
static int wait_resp(cast_conn *c, int id, int timeout_ms) {
  if (id < 0)
    return -1;
  int64_t deadline = now_ms() + timeout_ms;
  while (!c->got_resp || c->wait_req != id) {
    int left = (int)(deadline - now_ms());
    if (left <= 0) {
      LOGW("%s: request %d timed out", c->tag, id);
      return -1;
    }
    if (cast_pump(c, left > 200 ? 200 : left) < 0)
      return -1;
  }
  return 0;
}

int cast_open(cast_conn *c, const char *tag, const char *host, int port, int timeout_ms) {
  cast_global_init();
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  snprintf(c->tag, sizeof(c->tag), "%s", tag);
  snprintf(c->host, sizeof(c->host), "%s", host);
  c->port = port;

  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);
  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *ai = NULL;
  if (getaddrinfo(host, portstr, &hints, &ai) != 0 || !ai) {
    LOGW("%s: cannot resolve %s", tag, host);
    return -1;
  }
  c->fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (c->fd < 0) {
    freeaddrinfo(ai);
    return -1;
  }
  fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL) | O_NONBLOCK);
  int one = 1;
  setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  int r = connect(c->fd, ai->ai_addr, ai->ai_addrlen);
  freeaddrinfo(ai);
  int64_t deadline = now_ms() + timeout_ms;
  if (r < 0 && errno != EINPROGRESS)
    goto fail;
  if (r < 0) {
    if (wait_fd(c->fd, POLLOUT, timeout_ms) <= 0)
      goto fail;
    int err = 0;
    socklen_t el = sizeof(err);
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) {
      errno = err;
      goto fail;
    }
  }
  c->ssl = SSL_new(g_ctx);
  SSL_set_fd(c->ssl, c->fd);
  for (;;) {
    int hr = SSL_connect(c->ssl);
    if (hr == 1)
      break;
    int err = SSL_get_error(c->ssl, hr);
    int left = (int)(deadline - now_ms());
    if (left <= 0 || (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE))
      goto fail;
    wait_fd(c->fd, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, left);
  }
  c->last_rx_ms = c->last_ping_ms = now_ms();
  if (send_raw(c, RECEIVER_ID, NS_CONNECTION, "{\"type\":\"CONNECT\"}") < 0)
    goto fail;
  if (cast_get_status(c, (int)(deadline - now_ms()) > 1000 ? (int)(deadline - now_ms()) : 1000) <
      0)
    goto fail;
  LOGI("%s: connected to %s:%d (volume %.2f%s, app %s)", tag, host, port, c->volume,
       c->muted ? " muted" : "", c->app_id[0] ? c->app_id : "none");
  return 0;
fail:
  LOGW("%s: connect to %s:%d failed: %s", tag, host, port, strerror(errno));
  teardown(c);
  return -1;
}

void cast_close(cast_conn *c) {
  if (!c->ssl)
    return;
  if (c->app_connected && c->transport_id[0])
    send_raw(c, c->transport_id, NS_CONNECTION, "{\"type\":\"CLOSE\"}");
  send_raw(c, RECEIVER_ID, NS_CONNECTION, "{\"type\":\"CLOSE\"}");
  SSL_shutdown(c->ssl);
  teardown(c);
}

int cast_get_status(cast_conn *c, int timeout_ms) {
  return wait_resp(c, send_req(c, RECEIVER_ID, NS_RECEIVER, "{\"type\":\"GET_STATUS\"}"),
                   timeout_ms);
}

static int connect_app(cast_conn *c) {
  if (c->app_connected)
    return 0;
  if (!c->transport_id[0])
    return -1;
  if (send_raw(c, c->transport_id, NS_CONNECTION, "{\"type\":\"CONNECT\"}") < 0)
    return -1;
  c->app_connected = 1;
  return 0;
}

int cast_launch(cast_conn *c, const char *app_id, int timeout_ms) {
  int64_t deadline = now_ms() + timeout_ms;
  if (strcmp(c->app_id, app_id) || !c->transport_id[0]) {
    char req[128];
    snprintf(req, sizeof(req), "{\"type\":\"LAUNCH\",\"appId\":\"%s\"}", app_id);
    if (wait_resp(c, send_req(c, RECEIVER_ID, NS_RECEIVER, req), timeout_ms) < 0)
      return -1;
    if (strcmp(c->resp_type, "RECEIVER_STATUS")) {
      LOGW("%s: launch failed: %s", c->tag, c->resp_type);
      return -1;
    }
    /* the app may appear in a later status update */
    while (strcmp(c->app_id, app_id) || !c->transport_id[0]) {
      int left = (int)(deadline - now_ms());
      if (left <= 0) {
        LOGW("%s: application %s did not start", c->tag, app_id);
        return -1;
      }
      if (cast_pump(c, left > 200 ? 200 : left) < 0)
        return -1;
    }
    LOGI("%s: launched %s (session %s)", c->tag, app_id, c->session_id);
  }
  return connect_app(c);
}

int cast_load(cast_conn *c, const char *url, const char *content_type, const char *title,
              const char *artist, int timeout_ms) {
  if (connect_app(c) < 0)
    return -1;
  char et[512], ea[512], eu[1024], req[4096];
  json_escape(et, sizeof(et), title ? title : "AirPlay");
  json_escape(ea, sizeof(ea), artist ? artist : "");
  json_escape(eu, sizeof(eu), url);
  snprintf(req, sizeof(req),
           "{\"type\":\"LOAD\",\"autoplay\":true,\"currentTime\":0,"
           "\"media\":{\"contentId\":\"%s\",\"contentType\":\"%s\",\"streamType\":\"LIVE\","
           "\"metadata\":{\"metadataType\":3,\"title\":\"%s\",\"artist\":\"%s\"}}}",
           eu, content_type, et, ea);
  if (wait_resp(c, send_req(c, c->transport_id, NS_MEDIA, req), timeout_ms) < 0)
    return -1;
  if (strcmp(c->resp_type, "MEDIA_STATUS")) {
    LOGW("%s: load failed: %s", c->tag, c->resp_type);
    return -1;
  }
  c->loaded_session_id = c->media_session_id;
  LOGI("%s: loaded %s (media session %ld, %s)", c->tag, url, c->media_session_id,
       c->player_state);
  return 0;
}

int cast_media_command(cast_conn *c, const char *type, int timeout_ms) {
  if (!c->media_session_id || !c->transport_id[0])
    return -1;
  char req[160];
  snprintf(req, sizeof(req), "{\"type\":\"%s\",\"mediaSessionId\":%ld}", type,
           c->media_session_id);
  return wait_resp(c, send_req(c, c->transport_id, NS_MEDIA, req), timeout_ms);
}

int cast_set_volume(cast_conn *c, double level, int timeout_ms) {
  char req[128];
  if (level < 0)
    level = 0;
  if (level > 1)
    level = 1;
  snprintf(req, sizeof(req), "{\"type\":\"SET_VOLUME\",\"volume\":{\"level\":%.3f}}", level);
  return wait_resp(c, send_req(c, RECEIVER_ID, NS_RECEIVER, req), timeout_ms);
}

int cast_stop_app(cast_conn *c, int timeout_ms) {
  if (!c->session_id[0])
    return 0;
  char req[160];
  snprintf(req, sizeof(req), "{\"type\":\"STOP\",\"sessionId\":\"%s\"}", c->session_id);
  int r = wait_resp(c, send_req(c, RECEIVER_ID, NS_RECEIVER, req), timeout_ms);
  c->app_connected = 0;
  return r;
}
