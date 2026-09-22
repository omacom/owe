#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "yyjson.h"

bool owe_drm_dpms_enabled(void);
/* Return -1 when connector data cannot establish the power state. */
int owe_drm_all_off(const char *root);
int owe_drm_connector_state(const char *root, const char *name);
bool owe_outputs_covered(const char *clients, const char *monitors, bool fullscreen_only);
bool owe_outputs_covered_docs(yyjson_doc *clients, yyjson_doc *monitors, bool fullscreen_only);
bool owe_outputs_covered_list(yyjson_doc *clients, yyjson_doc *monitors, bool fullscreen_only,
                              char *out, size_t out_len);
