/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Srini K */

#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

int log_level = L_INFO;

static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void logmsg(int level, const char *fmt, ...) {
  static const char *tags[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
  if (level > log_level)
    return;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm;
  localtime_r(&ts.tv_sec, &tm);
  pthread_mutex_lock(&log_mu);
  fprintf(stderr, "%02d:%02d:%02d.%03ld %s ", tm.tm_hour, tm.tm_min, tm.tm_sec,
          ts.tv_nsec / 1000000, tags[level]);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
  pthread_mutex_unlock(&log_mu);
}
