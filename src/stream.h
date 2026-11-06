#ifndef AP2C_STREAM_H
#define AP2C_STREAM_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* A live PCM stream (S16LE interleaved) fed by one AirPlay receiver and served over
 * HTTP as an endless WAV file to one or more Cast clients. */

typedef struct stream {
  char path[64]; /* URL path, e.g. "/stream/abcdef.wav" */
  int rate, channels;

  pthread_mutex_t mu;
  pthread_cond_t cv;
  uint8_t *buf;
  size_t cap;            /* ring capacity in bytes (multiple of the frame size) */
  uint64_t wpos;         /* total bytes ever written */
  uint64_t session_pos;  /* wpos when the current play session began */
  int64_t last_write_ms;
  unsigned generation;   /* bumping it disconnects current HTTP clients */
  int clients;
  int max_backlog_ms;    /* how far back a new client starts */
  int session_pending;   /* next write starts a new session */
  uint64_t client_sent;  /* bytes delivered to the most recent client */
  int registered;
} stream;

int stream_init(stream *s, const char *id, int rate, int channels, int ring_ms, int backlog_ms);
void stream_destroy(stream *s);
static inline size_t stream_frame_bytes(const stream *s) { return (size_t)s->channels * 2; }

/* Appends PCM; len must be a multiple of the frame size. */
void stream_write(stream *s, const uint8_t *data, size_t len);
void stream_mark_session_start(stream *s);
/* The next write (e.g. after a flush) begins a new session; new clients start there. */
void stream_mark_session_next(stream *s);
int64_t stream_last_write(stream *s);
void stream_kick_clients(stream *s);
int stream_client_count(stream *s);

/* Writes a WAV header for an endless stream into out (44 bytes). */
void wav_header(uint8_t out[44], int rate, int channels);

/* Picks the read position for a newly connected client. Exposed for tests. */
uint64_t stream_start_pos(const stream *s);

/* HTTP server: one listener serving every registered stream. */
int http_server_start(int port);   /* port 0 = ephemeral; returns bound port or -1 */
void http_server_stop(void);
void http_register(stream *s);
void http_unregister(stream *s);

#endif
