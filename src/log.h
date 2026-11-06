#ifndef AP2C_LOG_H
#define AP2C_LOG_H

#include <stdint.h>

enum { L_ERR = 0, L_WARN, L_INFO, L_DEBUG };

extern int log_level;

void logmsg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int64_t now_ms(void); /* CLOCK_MONOTONIC in milliseconds */

#define LOGE(...) logmsg(L_ERR, __VA_ARGS__)
#define LOGW(...) logmsg(L_WARN, __VA_ARGS__)
#define LOGI(...) logmsg(L_INFO, __VA_ARGS__)
#define LOGD(...) logmsg(L_DEBUG, __VA_ARGS__)

#endif
