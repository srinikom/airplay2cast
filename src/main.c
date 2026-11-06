/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */
/* airplay2cast: exposes every Google Cast device on the LAN as an AirPlay 2 receiver.
 *
 *   iPhone/Mac --AirPlay 2--> shairport-sync (one per Cast device, PCM on stdout)
 *              --> airplay2cast ring buffer --> HTTP WAV live stream --> Cast device
 *   airplay2cast drives the Cast device over Cast v2 (TLS :8009) with the Default
 *   Media Receiver, and maps AirPlay play/flush/volume events onto it.
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bridge.h"
#include "log.h"

#define VERSION "1.0.0"
#define MAX_DEVICES MDNS_MAX_DEVICES

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void usage(FILE *f) {
  fprintf(f,
          "airplay2cast " VERSION " - AirPlay 2 receivers for Google Cast devices\n\n"
          "usage: airplay2cast [run] [options]     run the bridge (default)\n"
          "       airplay2cast discover [options]  list Cast devices and exit\n"
          "       airplay2cast tone NAME [options] play a test tone on a device (no AirPlay)\n\n"
          "options:\n"
          "  -s, --shairport PATH     shairport-sync executable (default: shairport-sync)\n"
          "  -d, --state-dir DIR      directory for generated configs (default: $XDG_RUNTIME_DIR/airplay2cast)\n"
          "  -p, --http-port N        HTTP stream port (default 8089)\n"
          "  -a, --http-ip IP         address Cast devices use to reach us (default: auto)\n"
          "  -b, --base-port N        first AirPlay RTSP port (default 7000)\n"
          "  -i, --include LIST       comma-separated device names/ids to bridge (default: all)\n"
          "  -x, --exclude LIST       comma-separated device names/ids to skip\n"
          "  -G, --no-groups          do not bridge Cast speaker groups\n"
          "  -S, --suffix TEXT        appended to AirPlay names (default: none)\n"
          "  -t, --idle-timeout S     release the speaker after S seconds without audio (default 30)\n"
          "  -l, --backlog MS         audio a newly connected speaker starts with (default 1000)\n"
          "  -D, --duck PERCENT       launch-chime volume cap, 0 disables (default 5)\n"
          "  -H, --hw-volume          AirPlay volume sets the speaker volume (default: software gain)\n"
          "  -F, --no-flush-stop      keep speaker media running across AirPlay pause/skip\n"
          "  -m, --mdns-if IP         IPv4 address of the interface used for discovery\n"
          "  -I, --scan-interval S    rediscovery interval (default 30)\n"
          "      --seconds N          tone duration (default 10)\n"
          "  -v, --verbose            debug logging (twice: shairport-sync debug too)\n"
          "  -V, --version\n"
          "  -h, --help\n");
}

static int list_has(const char *list, const cast_device *d) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s", list);
  for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    while (*tok == ' ')
      tok++;
    size_t l = strlen(tok);
    while (l && tok[l - 1] == ' ')
      tok[--l] = 0;
    if (*tok && (!strcasecmp(tok, d->name) || !strcasecmp(tok, d->id)))
      return 1;
  }
  return 0;
}

static int wanted(const bridge_config *cfg, const cast_device *d) {
  if (!cfg->include_groups && !strcmp(d->model, "Google Cast Group"))
    return 0;
  if (cfg->include[0] && !list_has(cfg->include, d))
    return 0;
  if (cfg->exclude[0] && list_has(cfg->exclude, d))
    return 0;
  return 1;
}

static int mkdir_p(const char *path) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0700);
      *p = '/';
    }
  return mkdir(tmp, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static int cmd_discover(bridge_config *cfg) {
  cast_device devs[MAX_DEVICES];
  int n = mdns_browse_cast(devs, MAX_DEVICES, 3000, cfg->mdns_if);
  if (n < 0) {
    LOGE("discovery failed");
    return 1;
  }
  printf("%-28s %-18s %-16s %-6s %s\n", "NAME", "MODEL", "ADDRESS", "PORT", "ID");
  for (int i = 0; i < n; i++)
    printf("%-28s %-18s %-16s %-6d %s%s\n", devs[i].name, devs[i].model, devs[i].ip, devs[i].port,
           devs[i].id, wanted(cfg, &devs[i]) ? "" : "  (excluded)");
  return 0;
}

static int find_device(bridge_config *cfg, const char *name, cast_device *out) {
  cast_device devs[MAX_DEVICES];
  int n = mdns_browse_cast(devs, MAX_DEVICES, 3000, cfg->mdns_if);
  for (int i = 0; i < n; i++)
    if (!strcasecmp(devs[i].name, name) || !strcasecmp(devs[i].id, name)) {
      *out = devs[i];
      return 0;
    }
  return -1;
}

static int cmd_tone(bridge_config *cfg, const char *name, int seconds) {
  cast_device dev;
  if (find_device(cfg, name, &dev) < 0) {
    LOGE("Cast device \"%s\" not found", name);
    return 1;
  }
  int port = http_server_start(cfg->http_port);
  if (port < 0)
    return 1;
  cfg->http_port = port;
  int rc = bridge_cast_test(cfg, &dev, seconds, 440.0);
  http_server_stop();
  LOGI("tone test %s", rc == 0 ? "PASSED (receiver reported PLAYING)" : "FAILED");
  return rc == 0 ? 0 : 1;
}

static int cmd_run(bridge_config *cfg) {
  if (mkdir_p(cfg->state_dir) < 0) {
    LOGE("cannot create state dir %s: %s", cfg->state_dir, strerror(errno));
    return 1;
  }
  if (access(cfg->sps_bin, X_OK) < 0 && !strchr(cfg->sps_bin, '/')) {
    /* resolve through PATH so failures are reported up front */
    char *path = getenv("PATH"), buf[1024];
    snprintf(buf, sizeof(buf), "%s", path ? path : "/usr/local/bin:/usr/bin:/bin");
    int found = 0;
    for (char *save, *dir = strtok_r(buf, ":", &save); dir && !found;
         dir = strtok_r(NULL, ":", &save)) {
      char cand[512];
      snprintf(cand, sizeof(cand), "%s/%s", dir, cfg->sps_bin);
      if (access(cand, X_OK) == 0) {
        snprintf(cfg->sps_bin, sizeof(cfg->sps_bin), "%s", cand);
        found = 1;
      }
    }
  }
  if (access(cfg->sps_bin, X_OK) < 0) {
    LOGE("shairport-sync not found at \"%s\" (use --shairport)", cfg->sps_bin);
    return 1;
  }
  int port = http_server_start(cfg->http_port);
  if (port < 0)
    return 1;
  cfg->http_port = port;
  LOGI("airplay2cast " VERSION " using %s, state in %s", cfg->sps_bin, cfg->state_dir);

  device *devs[MAX_DEVICES] = {0};
  int64_t next_scan = 0;
  int scans = 0;
  while (!g_stop) {
    if (now_ms() >= next_scan) {
      cast_device found[MAX_DEVICES];
      int n = mdns_browse_cast(found, MAX_DEVICES, 2500, cfg->mdns_if);
      int seen[MAX_DEVICES] = {0};
      for (int i = 0; i < n && !g_stop; i++) {
        if (!wanted(cfg, &found[i]))
          continue;
        int slot = -1, free_slot = -1;
        for (int k = 0; k < MAX_DEVICES; k++) {
          if (devs[k] && !strcmp(devs[k]->info.id, found[i].id))
            slot = k;
          else if (!devs[k] && free_slot < 0)
            free_slot = k;
        }
        if (slot >= 0) {
          device *d = devs[slot];
          seen[slot] = 1;
          d->missed_scans = 0;
          pthread_mutex_lock(&d->mu);
          if (strcmp(d->info.ip, found[i].ip) || d->info.port != found[i].port) {
            LOGI("%s: moved to %s:%d", d->tag, found[i].ip, found[i].port);
            snprintf(d->info.ip, sizeof(d->info.ip), "%s", found[i].ip);
            d->info.port = found[i].port;
          }
          pthread_mutex_unlock(&d->mu);
        } else if (free_slot >= 0) {
          devs[free_slot] = device_start(&found[i], free_slot, cfg);
          seen[free_slot] = 1;
        }
      }
      for (int k = 0; k < MAX_DEVICES; k++)
        if (devs[k] && !seen[k] && ++devs[k]->missed_scans >= 4 && !device_is_busy(devs[k])) {
          LOGI("%s: no longer discovered", devs[k]->tag);
          device_stop(devs[k]);
          devs[k] = NULL;
        }
      /* a quick second scan after startup catches devices whose replies were missed */
      next_scan = now_ms() + (++scans == 1 ? 5000 : cfg->discovery_interval_s * 1000LL);
    }
    usleep(200000);
  }
  LOGI("shutting down");
  for (int k = 0; k < MAX_DEVICES; k++)
    if (devs[k])
      devs[k]->stop = 1; /* let all devices release in parallel */
  for (int k = 0; k < MAX_DEVICES; k++)
    if (devs[k])
      device_stop(devs[k]);
  http_server_stop();
  return 0;
}

int main(int argc, char **argv) {
  bridge_config cfg = {
      .http_port = 8089,
      .base_port = 7000,
      .include_groups = 1,
      .idle_timeout_s = 30,
      .ring_ms = 10000,
      .backlog_ms = 1000,
      .rate = 48000,
      .duck_percent = 5,
      .flush_stop = 1,
      .discovery_interval_s = 30,
  };
  snprintf(cfg.sps_bin, sizeof(cfg.sps_bin), "shairport-sync");
  const char *rt = getenv("XDG_RUNTIME_DIR");
  if (rt && *rt)
    snprintf(cfg.state_dir, sizeof(cfg.state_dir), "%s/airplay2cast", rt);
  else
    snprintf(cfg.state_dir, sizeof(cfg.state_dir), "/tmp/airplay2cast-%d", (int)getuid());

  /* environment overrides (handy for containers) */
  const char *e;
  if ((e = getenv("AP2C_INCLUDE")))
    snprintf(cfg.include, sizeof(cfg.include), "%s", e);
  if ((e = getenv("AP2C_EXCLUDE")))
    snprintf(cfg.exclude, sizeof(cfg.exclude), "%s", e);

  enum { OPT_SECONDS = 1000 };
  static const struct option opts[] = {
      {"shairport", required_argument, 0, 's'}, {"state-dir", required_argument, 0, 'd'},
      {"http-port", required_argument, 0, 'p'}, {"http-ip", required_argument, 0, 'a'},
      {"base-port", required_argument, 0, 'b'}, {"include", required_argument, 0, 'i'},
      {"exclude", required_argument, 0, 'x'},   {"no-groups", no_argument, 0, 'G'},
      {"suffix", required_argument, 0, 'S'},    {"idle-timeout", required_argument, 0, 't'},
      {"backlog", required_argument, 0, 'l'},   {"duck", required_argument, 0, 'D'},
      {"hw-volume", no_argument, 0, 'H'},       {"no-flush-stop", no_argument, 0, 'F'},
      {"mdns-if", required_argument, 0, 'm'},   {"scan-interval", required_argument, 0, 'I'},
      {"seconds", required_argument, 0, OPT_SECONDS},
      {"verbose", no_argument, 0, 'v'},         {"version", no_argument, 0, 'V'},
      {"help", no_argument, 0, 'h'},            {0, 0, 0, 0}};
  int seconds = 10, c;
  while ((c = getopt_long(argc, argv, "s:d:p:a:b:i:x:GS:t:l:D:HFm:I:vVh", opts, NULL)) != -1) {
    switch (c) {
    case 's': snprintf(cfg.sps_bin, sizeof(cfg.sps_bin), "%s", optarg); break;
    case 'd': snprintf(cfg.state_dir, sizeof(cfg.state_dir), "%s", optarg); break;
    case 'p': cfg.http_port = atoi(optarg); break;
    case 'a': snprintf(cfg.http_ip, sizeof(cfg.http_ip), "%s", optarg); break;
    case 'b': cfg.base_port = atoi(optarg); break;
    case 'i': snprintf(cfg.include, sizeof(cfg.include), "%s", optarg); break;
    case 'x': snprintf(cfg.exclude, sizeof(cfg.exclude), "%s", optarg); break;
    case 'G': cfg.include_groups = 0; break;
    case 'S': snprintf(cfg.suffix, sizeof(cfg.suffix), "%s", optarg); break;
    case 't': cfg.idle_timeout_s = atoi(optarg); break;
    case 'l': cfg.backlog_ms = atoi(optarg); break;
    case 'D': cfg.duck_percent = atoi(optarg); break;
    case 'H': cfg.hw_volume = 1; break;
    case 'F': cfg.flush_stop = 0; break;
    case 'm': snprintf(cfg.mdns_if, sizeof(cfg.mdns_if), "%s", optarg); break;
    case 'I': cfg.discovery_interval_s = atoi(optarg); break;
    case OPT_SECONDS: seconds = atoi(optarg); break;
    case 'v':
      if (log_level == L_DEBUG)
        cfg.sps_verbosity = 1;
      log_level = L_DEBUG;
      break;
    case 'V': puts("airplay2cast " VERSION); return 0;
    case 'h': usage(stdout); return 0;
    default: usage(stderr); return 2;
    }
  }
  if (cfg.duck_percent < 0 || cfg.duck_percent > 100 || cfg.idle_timeout_s < 1 ||
      cfg.backlog_ms < 0 || cfg.discovery_interval_s < 5 || cfg.http_port < 0 ||
      cfg.http_port > 65535 || cfg.base_port < 1 || cfg.base_port > 65000) {
    fprintf(stderr, "invalid option value\n");
    return 2;
  }

  struct sigaction sa = {.sa_handler = on_signal};
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  cast_global_init();

  const char *cmd = optind < argc ? argv[optind] : "run";
  if (!strcmp(cmd, "run"))
    return cmd_run(&cfg);
  if (!strcmp(cmd, "discover"))
    return cmd_discover(&cfg);
  if (!strcmp(cmd, "tone")) {
    if (optind + 1 >= argc) {
      fprintf(stderr, "tone: device name required\n");
      return 2;
    }
    return cmd_tone(&cfg, argv[optind + 1], seconds);
  }
  usage(stderr);
  return 2;
}
