#include "debug_log.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int s_dbg_sock = -1;
static struct sockaddr_in s_dbg_addr;
static int s_dbg_enabled = 0;

void dbg_init(const char *target_ip, int target_port) {
    if (!target_ip || target_port <= 0) return;

    s_dbg_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_dbg_sock < 0) return;

    memset(&s_dbg_addr, 0, sizeof(s_dbg_addr));
    s_dbg_addr.sin_family = AF_INET;
    s_dbg_addr.sin_port = htons(target_port);
    s_dbg_addr.sin_addr.s_addr = inet_addr(target_ip);
    s_dbg_enabled = 1;

    dbg_log("[PS4-DBG] Telemetria UDP attiva -> %s:%d\n", target_ip, target_port);
}

void dbg_log(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len <= 0) return;

    /* Always print to stdout (GoldHEN Klog port 9998) */
    printf("%s", buf);
    fflush(stdout);

    /* Also send over UDP to PC for instant live debugging */
    if (s_dbg_enabled && s_dbg_sock >= 0) {
        sendto(s_dbg_sock, buf, len, 0, (struct sockaddr *)&s_dbg_addr, sizeof(s_dbg_addr));
    }
}
