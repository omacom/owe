#include "json.h"
#include <limits.h>
#include <string.h>

char *owe_json_quote(const char *value) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *str = yyjson_mut_strcpy(doc, value ? value : "");
    char *out = str ? yyjson_mut_val_write(str, 0, NULL) : NULL;
    yyjson_mut_doc_free(doc);
    return out;
}

bool owe_json_ok(const char *reply) {
    yyjson_doc *doc = reply ? yyjson_read(reply, strlen(reply), 0) : NULL;
    if (!doc) return false;
    yyjson_val *status = yyjson_obj_get(yyjson_doc_get_root(doc), "status");
    bool ok = yyjson_equals_str(status, "ok");
    yyjson_doc_free(doc);
    return ok;
}

bool owe_json_path(yyjson_val *value) {
    if (!yyjson_is_str(value)) return false;
    size_t len = yyjson_get_len(value);
    return len > 0 && len < PATH_MAX && strlen(yyjson_get_str(value)) == len;
}
