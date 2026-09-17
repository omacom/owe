#pragma once
#include <stdbool.h>

#include "yyjson.h"

/* Return -1 when connector data cannot establish the power state. */
int owe_drm_all_off(const char *root);
bool owe_outputs_covered(const char *clients, const char *monitors, bool fullscreen_only);
bool owe_outputs_covered_docs(yyjson_doc *clients, yyjson_doc *monitors, bool fullscreen_only);
