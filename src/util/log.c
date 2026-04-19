/*
 * log.c - Minimal leveled logging to stderr.
 */
#include "log.h"

#include <stdarg.h>

static nebula_log_level_t g_level = NEBULA_LOG_INFO;

static const char *lvl_str(nebula_log_level_t l)
{
    switch (l) {
    case NEBULA_LOG_ERROR: return "ERROR";
    case NEBULA_LOG_WARN:  return "WARN ";
    case NEBULA_LOG_INFO:  return "INFO ";
    case NEBULA_LOG_DEBUG: return "DEBUG";
    default:               return "?????";
    }
}

void nebula_log_set_level(nebula_log_level_t lvl)
{
    g_level = lvl;
}

void nebula_log_msg(nebula_log_level_t lvl, const char *fmt, ...)
{
    if (lvl > g_level) return;
    fprintf(stderr, "[%s] ", lvl_str(lvl));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
