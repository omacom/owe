#include "log.h"

#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *g_tag = "owe";
static owe_log_level_t g_level = OWE_LOG_INFO;

void owe_log_init(const char *tag, owe_log_level_t level) {
    if (tag) {
        g_tag = tag;
    }
    g_level = level;
}

void owe_log_set_level(owe_log_level_t level) {
    g_level = level;
}

void owe_log(owe_log_level_t level, const char *file, int line, const char *fmt, ...) {
    static const char *names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    FILE *out;
    struct timespec ts;
    struct tm tm;
    char tbuf[32];
    va_list ap;

    if (level < g_level) {
        return;
    }
    out = level >= OWE_LOG_WARN ? stderr : stdout;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm);
    fprintf(out, "%s.%03ld %s[%d] %s %s:%d: ", tbuf, ts.tv_nsec / 1000000L, g_tag,
            (int)getpid(), names[level], file, line);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}
