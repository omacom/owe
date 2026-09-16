#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OWE_KIND_UNKNOWN = 0,
    OWE_KIND_STILL = 1,
    OWE_KIND_VIDEO = 2,
    OWE_KIND_GIF = 3,
} owe_media_kind_t;

owe_media_kind_t owe_kind_from_path(const char *path);
const char *owe_kind_to_string(owe_media_kind_t kind);

bool owe_has_suffix_ci(const char *path, const char *suffix);
void owe_trim(char *s);
