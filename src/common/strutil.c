#include "strutil.h"

#include <ctype.h>
#include <string.h>

bool owe_has_suffix_ci(const char *path, const char *suffix) {
    size_t plen;
    size_t slen;
    size_t i;
    if (!path || !suffix) {
        return false;
    }
    plen = strlen(path);
    slen = strlen(suffix);
    if (slen == 0 || slen > plen) {
        return false;
    }
    for (i = 0; i < slen; i++) {
        if (tolower((unsigned char)path[plen - slen + i]) !=
            tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

owe_media_kind_t owe_kind_from_path(const char *path) {
    static const char *video[] = {
        ".mp4",  ".m4v", ".mov", ".webm", ".mkv", ".avi", ".mpeg", ".mpg", ".ts",
        ".m2ts", ".mts", ".wmv", ".asf",  ".flv", ".f4v", ".ogv",  ".3gp", ".3g2",
        ".apng", NULL,
    };
    static const char *still[] = { ".jpg", ".jpeg", ".png", ".bmp", ".webp", ".tif",
                                   ".tiff", ".avif", ".jxl", ".heic", ".heif", NULL };
    size_t i;
    if (!path || !*path) {
        return OWE_KIND_UNKNOWN;
    }
    if (owe_has_suffix_ci(path, ".gif")) {
        return OWE_KIND_GIF;
    }
    for (i = 0; video[i]; i++) {
        if (owe_has_suffix_ci(path, video[i])) {
            return OWE_KIND_VIDEO;
        }
    }
    for (i = 0; still[i]; i++) {
        if (owe_has_suffix_ci(path, still[i])) {
            return OWE_KIND_STILL;
        }
    }
    return OWE_KIND_UNKNOWN;
}

const char *owe_kind_to_string(owe_media_kind_t kind) {
    switch (kind) {
    case OWE_KIND_STILL:
        return "still";
    case OWE_KIND_VIDEO:
        return "video";
    case OWE_KIND_GIF:
        return "gif";
    default:
        return "unknown";
    }
}

void owe_trim(char *s) {
    char *end;
    char *start = s;
    if (!s) {
        return;
    }
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
}
