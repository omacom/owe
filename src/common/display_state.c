#include "display_state.h"
#include "yyjson.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int attribute(const char *root, const char *name, const char *key, char *out, size_t size) {
    char *path = NULL;
    if (asprintf(&path, "%s/%s/%s", root, name, key) < 0) return -1;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return -1;
    bool ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return ok ? 0 : -1;
}

bool owe_drm_dpms_enabled(void) {
    const char *value = getenv("OWE_DRM_DPMS");
    return !value || strcmp(value, "0") != 0;
}

int owe_drm_all_off(const char *root) {
    if (!owe_drm_dpms_enabled()) return -1;
    DIR *dir = opendir(root);
    if (!dir) return -1;
    struct dirent *entry;
    bool known = false, unknown = false;
    int result = -1;
    while ((entry = readdir(dir))) {
        if (!strchr(entry->d_name, '-')) continue;
        char status[64] = "", dpms[64] = "";
        if (attribute(root, entry->d_name, "status", status, sizeof(status)) < 0) continue;
        if (strcmp(status, "disconnected") == 0) continue;
        if (strcmp(status, "connected") != 0 ||
            attribute(root, entry->d_name, "dpms", dpms, sizeof(dpms)) < 0) {
            unknown = true;
            continue;
        }
        if (strcmp(dpms, "On") == 0) { result = 0; goto done; }
        if (strcmp(dpms, "Off") && strcmp(dpms, "Standby") && strcmp(dpms, "Suspend"))
            unknown = true;
        else
            known = true;
    }
    if (known && !unknown) result = 1;
done:
    closedir(dir);
    return result;
}

/* 1 when the named connector is off, 0 when it is on, -1 when unknown. */
int owe_drm_connector_state(const char *root, const char *name) {
    DIR *dir;
    struct dirent *entry;
    char suffix[300];
    int result = -1;
    bool matched = false;
    if (!owe_drm_dpms_enabled()) return -1;
    if (!root || !name || !*name) return -1;
    snprintf(suffix, sizeof(suffix), "-%s", name);
    dir = opendir(root);
    if (!dir) return -1;
    while ((entry = readdir(dir))) {
        char status[64] = "";
        char dpms[64] = "";
        size_t len = strlen(entry->d_name);
        size_t slen = strlen(suffix);
        if (len < slen || strcmp(entry->d_name + len - slen, suffix) != 0) continue;
        if (attribute(root, entry->d_name, "status", status, sizeof(status)) < 0) continue;
        if (strcmp(status, "connected") != 0) continue;
        /* Connector names can repeat across GPUs. A name alone cannot select the GPU. */
        if (matched) {
            result = -1;
            break;
        }
        matched = true;
        if (attribute(root, entry->d_name, "dpms", dpms, sizeof(dpms)) < 0) {
            result = -1;
            continue;
        }
        if (strcmp(dpms, "On") == 0) {
            result = 0;
        } else if (strcmp(dpms, "Off") == 0 || strcmp(dpms, "Standby") == 0 ||
                   strcmp(dpms, "Suspend") == 0) {
            result = 1;
        } else {
            result = -1;
        }
    }
    closedir(dir);
    return result;
}

static bool flag(yyjson_val *value) {
    return yyjson_is_bool(value) ? yyjson_get_bool(value) : yyjson_get_int(value) != 0;
}

static bool monitor_covered(yyjson_val *monitor, yyjson_val *clients, bool fullscreen_only) {
    size_t j, jmax;
    yyjson_val *client;
    yyjson_val *dpms;
    yyjson_val *active;
    yyjson_val *special;
    if (!yyjson_is_obj(monitor)) return false;
    if (flag(yyjson_obj_get(monitor, "disabled"))) return false;
    dpms = yyjson_obj_get(monitor, "dpmsStatus");
    if (dpms && !flag(dpms)) return false;
    active = yyjson_obj_get(yyjson_obj_get(monitor, "activeWorkspace"), "id");
    special = yyjson_obj_get(yyjson_obj_get(monitor, "specialWorkspace"), "id");
    if (!yyjson_is_int(active)) return false;
    yyjson_arr_foreach(clients, j, jmax, client) {
        yyjson_val *mapped;
        yyjson_val *workspace;
        int id;
        if (flag(yyjson_obj_get(client, "hidden"))) continue;
        mapped = yyjson_obj_get(client, "mapped");
        if (mapped && !flag(mapped)) continue;
        workspace = yyjson_obj_get(yyjson_obj_get(client, "workspace"), "id");
        if (!yyjson_is_int(workspace)) continue;
        id = yyjson_get_int(workspace);
        if (id != yyjson_get_int(active) &&
            (!special || !yyjson_get_int(special) || id != yyjson_get_int(special))) continue;
        if (fullscreen_only && yyjson_get_int(yyjson_obj_get(client, "fullscreen")) != 2) continue;
        return true;
    }
    return false;
}

bool owe_outputs_covered_docs(yyjson_doc *c, yyjson_doc *m, bool fullscreen_only) {
    yyjson_val *cr = c ? yyjson_doc_get_root(c) : NULL;
    yyjson_val *mr = m ? yyjson_doc_get_root(m) : NULL;
    bool covered = false;
    size_t i, imax;
    yyjson_val *monitor;
    if (!yyjson_is_arr(cr) || !yyjson_is_arr(mr)) return false;
    yyjson_arr_foreach(mr, i, imax, monitor) {
        if (flag(yyjson_obj_get(monitor, "disabled"))) continue;
        yyjson_val *dpms = yyjson_obj_get(monitor, "dpmsStatus");
        if (dpms && !flag(dpms)) continue;
        if (!monitor_covered(monitor, cr, fullscreen_only)) return false;
        covered = true;
    }
    return covered;
}

/* Comma separated names of monitors where a window covers the output. */
bool owe_outputs_covered_list(yyjson_doc *c, yyjson_doc *m, bool fullscreen_only, char *out,
                              size_t out_len) {
    yyjson_val *cr = c ? yyjson_doc_get_root(c) : NULL;
    yyjson_val *mr = m ? yyjson_doc_get_root(m) : NULL;
    size_t i, imax;
    yyjson_val *monitor;
    size_t used = 0;
    if (!out || out_len < 2) return false;
    out[0] = '\0';
    if (!yyjson_is_arr(cr) || !yyjson_is_arr(mr)) return false;
    yyjson_arr_foreach(mr, i, imax, monitor) {
        yyjson_val *name;
        if (flag(yyjson_obj_get(monitor, "disabled"))) continue;
        yyjson_val *dpms = yyjson_obj_get(monitor, "dpmsStatus");
        if (dpms && !flag(dpms)) continue;
        if (!monitor_covered(monitor, cr, fullscreen_only)) continue;
        name = yyjson_obj_get(monitor, "name");
        if (!yyjson_is_str(name)) continue;
        used += (size_t)snprintf(out + used, out_len - used, "%s%s",
                                 used ? "," : "", yyjson_get_str(name));
        if (used >= out_len) {
            out[out_len - 1] = '\0';
            return true;
        }
    }
    return out[0] != '\0';
}

bool owe_outputs_covered(const char *clients, const char *monitors, bool fullscreen_only) {
    yyjson_doc *c = clients ? yyjson_read(clients, strlen(clients), 0) : NULL;
    yyjson_doc *m = monitors ? yyjson_read(monitors, strlen(monitors), 0) : NULL;
    bool covered = owe_outputs_covered_docs(c, m, fullscreen_only);
    yyjson_doc_free(c);
    yyjson_doc_free(m);
    return covered;
}
