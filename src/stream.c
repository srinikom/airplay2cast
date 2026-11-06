#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#define MAX_STREAMS 64
#define CHUNK 8192
#define SILENCE_AFTER_MS 400 /* gap after which silence is inserted to keep the client fed */
#define SILENCE_SLICE_MS 20

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

void wav_header(uint8_t h[44], int rate, int channels) {
  memcpy(h, "RIFF", 4);
  put_le32(h + 4, 0xFFFFFFFFu); /* unknown length: maximum */
  memcpy(h + 8, "WAVEfmt ", 8);
  put_le32(h + 16, 16);
  put_le16(h + 20, 1); /* PCM */
  put_le16(h + 22, (uint16_t)channels);
  put_le32(h + 24, (uint32_t)rate);
  put_le32(h + 28, (uint32_t)(rate * channels * 2));
  put_le16(h + 32, (uint16_t)(channels * 2));
  put_le16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  put_le32(h + 40, 0xFFFFFFFFu - 36);
}

int stream_init(stream *s, const char *id, int rate, int channels, int ring_ms, int backlog_ms) {
  memset(s, 0, sizeof(*s));
  snprintf(s->path, sizeof(s->path), "/stream/%s.wav", id);
  s->rate = rate;
  s->channels = channels;
  size_t fb = stream_frame_bytes(s);
  s->cap = ((size_t)rate * (size_t)ring_ms / 1000) * fb;
  s->buf = malloc(s->cap);
  if (!s->buf)
    return -1;
  s->max_backlog_ms = backlog_ms < ring_ms - 500 ? backlog_ms : ring_ms - 500;
  pthread_mutex_init(&s->mu, NULL);
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&s->cv, &ca);
  pthread_condattr_destroy(&ca);
  return 0;
}

void stream_destroy(stream *s) {
  http_unregister(s);
  stream_kick_clients(s);
  pthread_mutex_lock(&s->mu);
  while (s->clients > 0) {
    s->generation++;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    usleep(20000);
    pthread_mutex_lock(&s->mu);
  }
  pthread_mutex_unlock(&s->mu);
  pthread_cond_destroy(&s->cv);
  pthread_mutex_destroy(&s->mu);
  free(s->buf);
  s->buf = NULL;
}

void stream_write(stream *s, const uint8_t *data, size_t len) {
  pthread_mutex_lock(&s->mu);
  /* audio resuming after a flush or a long gap starts a new session */
  if (s->session_pending || now_ms() - s->last_write_ms > 1000) {
    s->session_pos = s->wpos;
    s->session_pending = 0;
  }
  while (len) {
    size_t at = (size_t)(s->wpos % s->cap);
    size_t n = s->cap - at < len ? s->cap - at : len;
    memcpy(s->buf + at, data, n);
    s->wpos += n;
    data += n;
    len -= n;
  }
  s->last_write_ms = now_ms();
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
}

void stream_mark_session_start(stream *s) {
  pthread_mutex_lock(&s->mu);
  s->session_pos = s->wpos;
  pthread_mutex_unlock(&s->mu);
}

void stream_mark_session_next(stream *s) {
  pthread_mutex_lock(&s->mu);
  s->session_pending = 1;
  pthread_mutex_unlock(&s->mu);
}

int64_t stream_last_write(stream *s) {
  pthread_mutex_lock(&s->mu);
  int64_t t = s->last_write_ms;
  pthread_mutex_unlock(&s->mu);
  return t;
}

void stream_kick_clients(stream *s) {
  pthread_mutex_lock(&s->mu);
  s->generation++;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
}

int stream_client_count(stream *s) {
  pthread_mutex_lock(&s->mu);
  int n = s->clients;
  pthread_mutex_unlock(&s->mu);
  return n;
}

/* caller holds s->mu */
uint64_t stream_start_pos(const stream *s) {
  size_t fb = stream_frame_bytes(s);
  uint64_t backlog = (uint64_t)s->rate * (uint64_t)s->max_backlog_ms / 1000 * fb;
  uint64_t start = s->wpos > backlog ? s->wpos - backlog : 0;
  if (s->session_pos > start && s->session_pos <= s->wpos)
    start = s->session_pos;
  /* never older than what the ring still holds (keep a margin for the writer) */
  uint64_t oldest = s->wpos > s->cap - CHUNK ? s->wpos - (s->cap - CHUNK) : 0;
  if (start < oldest)
    start = oldest;
  return start - (start % fb);
}

/* ---------------------------------------------------------------- HTTP server */

static struct {
  pthread_mutex_t mu;
  stream *streams[MAX_STREAMS];
  int listen_fd;
  int port;
  volatile int running;
  pthread_t thread;
} srv = {.mu = PTHREAD_MUTEX_INITIALIZER, .listen_fd = -1};

void http_register(stream *s) {
  pthread_mutex_lock(&srv.mu);
  for (int i = 0; i < MAX_STREAMS; i++)
    if (!srv.streams[i]) {
      srv.streams[i] = s;
      s->registered = 1;
      break;
    }
  pthread_mutex_unlock(&srv.mu);
}

void http_unregister(stream *s) {
  pthread_mutex_lock(&srv.mu);
  for (int i = 0; i < MAX_STREAMS; i++)
    if (srv.streams[i] == s)
      srv.streams[i] = NULL;
  s->registered = 0;
  pthread_mutex_unlock(&srv.mu);
}

static int send_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = buf;
  while (len) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static void reply_simple(int fd, const char *status) {
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
                   "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
  send_all(fd, buf, (size_t)n);
}

typedef struct {
  int fd;
  char peer[64];
} client_arg;


static void serve_stream(int fd, stream *s, const char *peer, int head_only) {
  char hdr[512];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: audio/wav\r\n"
                   /* An explicit (maximal) length matters: without it, or with chunked
                    * encoding, the Cast media player underflows right after starting and
                    * rebuffers for ~8 s, adding that much latency. */
                   "Content-Length: 4294967295\r\n"
                   "Cache-Control: no-cache, no-store\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Connection: close\r\n"
                   "Server: airplay2cast\r\n\r\n");
  if (send_all(fd, hdr, (size_t)n) < 0 || head_only)
    return;
  uint8_t wav[44];
  wav_header(wav, s->rate, s->channels);
  if (send_all(fd, wav, sizeof(wav)) < 0)
    return;

  size_t fb = stream_frame_bytes(s);
  uint8_t *chunk = malloc(CHUNK);
  if (!chunk)
    return;
  /* silence slices are frame-aligned */
  size_t slice = (size_t)s->rate * SILENCE_SLICE_MS / 1000 * fb;
  if (slice > CHUNK)
    slice = CHUNK - (CHUNK % fb);

  pthread_mutex_lock(&s->mu);
  unsigned gen = s->generation;
  uint64_t pos = stream_start_pos(s);
  int64_t conn_ms = now_ms();
  LOGI("http: %s streaming %s (%.1f s backlog)", peer, s->path,
       (double)(s->wpos - pos) / (double)(fb * (size_t)s->rate));
  uint64_t sent = 0, silence = 0;
  s->client_sent = 0;
  int64_t silence_clock = 0;
  for (;;) {
    if (s->generation != gen)
      break;
    if (s->wpos > pos + s->cap - CHUNK) { /* client fell behind the ring; skip ahead */
      LOGW("http: %s fell behind, skipping %llu bytes", peer,
           (unsigned long long)(s->wpos - pos));
      pos = stream_start_pos(s);
    }
    size_t avail = (size_t)(s->wpos - pos);
    if (avail > 0) {
      size_t at = (size_t)(pos % s->cap);
      size_t len = avail < CHUNK ? avail : CHUNK;
      if (len > s->cap - at)
        len = s->cap - at;
      memcpy(chunk, s->buf + at, len);
      pthread_mutex_unlock(&s->mu);
      int r = send_all(fd, chunk, len);
      pthread_mutex_lock(&s->mu);
      if (r < 0)
        break;
      pos += len;
      sent += len;
      s->client_sent = sent + silence;
      silence_clock = 0;
      continue;
    }
    int64_t t = now_ms();
    if (t - s->last_write_ms >= SILENCE_AFTER_MS) {
      /* Source is paused or between tracks: feed silence so the receiver keeps the stream
       * open. Silence starts where the PCM stopped (the gap is filled first), so the
       * receiver's buffer depth -- and therefore latency -- stays constant. */
      if (!silence_clock)
        silence_clock = s->last_write_ms > conn_ms ? s->last_write_ms : conn_ms;
      if (t >= silence_clock) {
        pthread_mutex_unlock(&s->mu);
        memset(chunk, 0, slice);
        int r = send_all(fd, chunk, slice);
        pthread_mutex_lock(&s->mu);
        if (r < 0)
          break;
        silence += slice;
        s->client_sent = sent + silence;
        silence_clock += SILENCE_SLICE_MS;
        continue;
      }
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += 10 * 1000000;
    if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&s->cv, &s->mu, &ts);
  }
  pthread_mutex_unlock(&s->mu);
  free(chunk);
  LOGI("http: %s stopped %s (%.1f s audio, %.1f s silence)", peer, s->path,
       (double)sent / (double)(fb * (size_t)s->rate),
       (double)silence / (double)(fb * (size_t)s->rate));
}

static void *client_thread(void *arg) {
  client_arg *ca = arg;
  int fd = ca->fd;
  char req[2048];
  size_t len = 0;
  /* read the request head */
  while (len < sizeof(req) - 1) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 5000) <= 0)
      goto done;
    ssize_t n = recv(fd, req + len, sizeof(req) - 1 - len, 0);
    if (n <= 0)
      goto done;
    len += (size_t)n;
    req[len] = 0;
    if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
      break;
  }
  char method[16] = "", path[256] = "";
  if (sscanf(req, "%15s %255s", method, path) != 2) {
    reply_simple(fd, "400 Bad Request");
    goto done;
  }
  char *q = strchr(path, '?');
  if (q)
    *q = 0;
  int head = !strcasecmp(method, "HEAD");
  if (!head && strcasecmp(method, "GET")) {
    reply_simple(fd, "405 Method Not Allowed");
    goto done;
  }
  LOGD("http: %s %s %s", ca->peer, method, path);
  /* hold the registry lock only while looking up; the stream outlives its clients
   * because stream_destroy() waits for clients to drain */
  pthread_mutex_lock(&srv.mu);
  stream *s = NULL;
  for (int i = 0; i < MAX_STREAMS; i++)
    if (srv.streams[i] && !strcmp(srv.streams[i]->path, path)) {
      s = srv.streams[i];
      pthread_mutex_lock(&s->mu);
      s->clients++; /* pin */
      pthread_mutex_unlock(&s->mu);
      break;
    }
  pthread_mutex_unlock(&srv.mu);
  if (!s) {
    reply_simple(fd, "404 Not Found");
    goto done;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  serve_stream(fd, s, ca->peer, head);
  pthread_mutex_lock(&s->mu);
  s->clients--; /* unpin */
  pthread_mutex_unlock(&s->mu);
done:
  shutdown(fd, SHUT_RDWR);
  close(fd);
  free(ca);
  return NULL;
}

static void *accept_thread(void *unused) {
  (void)unused;
  while (srv.running) {
    struct pollfd p = {.fd = srv.listen_fd, .events = POLLIN};
    if (poll(&p, 1, 500) <= 0)
      continue;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    int fd = accept4(srv.listen_fd, (struct sockaddr *)&ss, &sl, SOCK_CLOEXEC);
    if (fd < 0)
      continue;
    client_arg *ca = calloc(1, sizeof(*ca));
    if (!ca) {
      close(fd);
      continue;
    }
    ca->fd = fd;
    if (ss.ss_family == AF_INET)
      inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, ca->peer, sizeof(ca->peer));
    else if (ss.ss_family == AF_INET6)
      inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, ca->peer, sizeof(ca->peer));
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    if (pthread_create(&t, &at, client_thread, ca) != 0) {
      close(fd);
      free(ca);
    }
    pthread_attr_destroy(&at);
  }
  return NULL;
}

int http_server_start(int port) {
  int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int v6 = 1;
  if (fd < 0) {
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    v6 = 0;
  }
  if (fd < 0)
    return -1;
  int one = 1, zero = 0;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int r;
  if (v6) {
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port),
                             .sin6_addr = in6addr_any};
    r = bind(fd, (struct sockaddr *)&a, sizeof(a));
  } else {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                            .sin_addr.s_addr = htonl(INADDR_ANY)};
    r = bind(fd, (struct sockaddr *)&a, sizeof(a));
  }
  if (r < 0 || listen(fd, 16) < 0) {
    LOGE("http: cannot listen on port %d: %s", port, strerror(errno));
    close(fd);
    return -1;
  }
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  getsockname(fd, (struct sockaddr *)&ss, &sl);
  srv.port = ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&ss)->sin6_port
                                            : ((struct sockaddr_in *)&ss)->sin_port);
  srv.listen_fd = fd;
  srv.running = 1;
  if (pthread_create(&srv.thread, NULL, accept_thread, NULL) != 0) {
    close(fd);
    srv.listen_fd = -1;
    return -1;
  }
  LOGI("http: serving streams on port %d", srv.port);
  return srv.port;
}

void http_server_stop(void) {
  if (!srv.running)
    return;
  srv.running = 0;
  pthread_join(srv.thread, NULL);
  close(srv.listen_fd);
  srv.listen_fd = -1;
}
