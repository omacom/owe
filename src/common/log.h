#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
    OWE_LOG_DEBUG = 0,
    OWE_LOG_INFO = 1,
    OWE_LOG_WARN = 2,
    OWE_LOG_ERROR = 3,
} owe_log_level_t;

void owe_log_init(const char *tag, owe_log_level_t level);
void owe_log_set_level(owe_log_level_t level);

void owe_log(owe_log_level_t level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define OWE_DEBUG(...) owe_log(OWE_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define OWE_INFO(...) owe_log(OWE_LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define OWE_WARN(...) owe_log(OWE_LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define OWE_ERROR(...) owe_log(OWE_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
