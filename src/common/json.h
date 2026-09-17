#pragma once
#include <stdbool.h>
#include "yyjson.h"

/* The caller frees the returned JSON string. */
char *owe_json_quote(const char *value);
bool owe_json_ok(const char *reply);
bool owe_json_path(yyjson_val *value);
