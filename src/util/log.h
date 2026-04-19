/*
 * log.h - Minimal leveled logging.
 */
#ifndef NEBULA_LOG_H
#define NEBULA_LOG_H

#include <stdio.h>

typedef enum {
    NEBULA_LOG_ERROR = 0,
    NEBULA_LOG_WARN  = 1,
    NEBULA_LOG_INFO  = 2,
    NEBULA_LOG_DEBUG = 3,
} nebula_log_level_t;

void nebula_log_set_level(nebula_log_level_t lvl);
void nebula_log_msg(nebula_log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define NEB_ERR(...)  nebula_log_msg(NEBULA_LOG_ERROR, __VA_ARGS__)
#define NEB_WARN(...) nebula_log_msg(NEBULA_LOG_WARN,  __VA_ARGS__)
#define NEB_INFO(...) nebula_log_msg(NEBULA_LOG_INFO,  __VA_ARGS__)
#define NEB_DBG(...)  nebula_log_msg(NEBULA_LOG_DEBUG, __VA_ARGS__)

#endif
