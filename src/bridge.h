/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */

#ifndef AP2C_BRIDGE_H
#define AP2C_BRIDGE_H

#include <pthread.h>
#include <sys/types.h>

#include "cast.h"
#include "mdns.h"
#include "stream.h"

typedef struct {
  char sps_bin[256];        /* shairport-sync executable */
  char state_dir[256];      /* generated shairport-sync configs */
  char http_ip[64];         /* IP used in stream URLs ("" = auto per device) */
  int http_port;            /* HTTP stream server port */
  int base_port;            /* first AirPlay RTSP port */
  char include[1024];       /* comma-separated friendly names / ids; empty = all */
  char exclude[1024];
  int include_groups;
  char suffix[64];          /* appended to AirPlay names */
  int idle_timeout_s;       /* stop the Cast app after this long without audio */
  int ring_ms, backlog_ms;
  int rate;
  int duck_percent;         /* chime suppression: launch at most this volume; 0 = off */
  int hw_volume;            /* AirPlay volume drives the speaker volume instead of PCM gain */
  int flush_stop;           /* stop Cast media on AirPlay flush (pause/skip) for responsiveness */
  char mdns_if[64];         /* IPv4 address of the interface used for discovery */
  int discovery_interval_s;
  int sps_verbosity;
} bridge_config;

typedef struct device {
  cast_device info;
  int slot;
  char tag[160];
  char airplay_name[160];
  const bridge_config *cfg;
  stream st;

  pthread_mutex_t mu;      /* guards info and the event fields below */
  volatile int stop;
  pthread_t audio_thr, ctl_thr;
  pid_t sps_pid;
  int meta_fd, meta_port;
  int missed_scans;

  /* AirPlay events (set by the control thread from metadata) */
  int ap_active;           /* between pbeg and pend */
  int prepared;            /* receiver start attempted for this AirPlay session */
  int flushed;             /* flush seen; reload needed when audio resumes */
  char title[256], artist[256];
  double ap_volume_db;     /* -30..0, or -144 for mute */
  int volume_dirty;

  /* Cast session state (control thread only) */
  cast_conn cc;
  int media_loaded;
  int ours;                /* we launched the current receiver application */
  int preempted;           /* another sender took the device; wait for a new session */
  int64_t next_retry_ms;
  int64_t launch_ms;
  double duck_saved;       /* volume to restore after the launch chime, <0 = none */
  int64_t duck_until_ms;
  int64_t load_ms;         /* when the current media was loaded */
  int64_t flush_ms;        /* last AirPlay flush; only audio written after it reloads */
  int playing_seen;        /* receiver reported PLAYING for the current load */
  int buffering_warned;
  int underrun;            /* waiting for PLAYING again after an underrun */
} device;

device *device_start(const cast_device *info, int slot, const bridge_config *cfg);
void device_stop(device *d);   /* stops threads, releases the speaker, frees */
int device_is_busy(device *d); /* audio session or cast session in progress */

/* Plays a generated test tone on a Cast device through the HTTP stream path. */
int bridge_cast_test(const bridge_config *cfg, const cast_device *dev, int seconds, double freq);

/* Local IP address that routes to `remote_ip` (for stream URLs). */
int local_ip_for(const char *remote_ip, char *out, size_t cap);
uint64_t airplay_device_id_for(const char *cast_id);

#endif
