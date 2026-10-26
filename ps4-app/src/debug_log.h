#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdio.h>
#include <stdarg.h>

void dbg_init(const char *target_ip, int target_port);
void dbg_log(const char *fmt, ...);

#endif /* DEBUG_LOG_H */
