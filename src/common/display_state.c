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

int owe_drm_all_off(const char *root) {
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

static bool flag(yyjson_val *value) {
    return yyjson_is_bool(value) ? yyjson_get_bool(value) : yyjson_get_int(value) != 0;
}

bool owe_outputs_covered_docs(yyjson_doc *c, yyjson_doc *m, bool fullscreen_only) {
    yyjson_val *cr = c ? yyjson_doc_get_root(c) : NULL;
    yyjson_val *mr = m ? yyjson_doc_get_root(m) : NULL;
    bool covered = false;
    if (!yyjson_is_arr(cr) || !yyjson_is_arr(mr)) return false;
    size_t i, imax, j, jmax;
    yyjson_val *monitor, *client;
    yyjson_arr_foreach(mr, i, imax, monitor) {
        if (flag(yyjson_obj_get(monitor, "disabled"))) continue;
        yyjson_val *dpms = yyjson_obj_get(monitor, "dpmsStatus");
        if (dpms && !flag(dpms)) continue;
        yyjson_val *active = yyjson_obj_get(yyjson_obj_get(monitor, "activeWorkspace"), "id");
        yyjson_val *special = yyjson_obj_get(yyjson_obj_get(monitor, "specialWorkspace"), "id");
        if (!yyjson_is_int(active)) { covered = false; return false; }
        bool here = false;
        yyjson_arr_foreach(cr, j, jmax, client) {
            if (flag(yyjson_obj_get(client, "hidden"))) continue;
            yyjson_val *mapped = yyjson_obj_get(client, "mapped");
            if (mapped && !flag(mapped)) continue;
            yyjson_val *workspace = yyjson_obj_get(yyjson_obj_get(client, "workspace"), "id");
            if (!yyjson_is_int(workspace)) continue;
            int id = yyjson_get_int(workspace);
            if (id != yyjson_get_int(active) &&
                (!special || !yyjson_get_int(special) || id != yyjson_get_int(special))) continue;
            if (fullscreen_only && yyjson_get_int(yyjson_obj_get(client, "fullscreen")) != 2) continue;
            here = true;
            break;
        }
        if (!here) { covered = false; return false; }
        covered = true;
    }
    return covered;
}

bool owe_outputs_covered(const char *clients, const char *monitors, bool fullscreen_only) {
    yyjson_doc *c = clients ? yyjson_read(clients, strlen(clients), 0) : NULL;
    yyjson_doc *m = monitors ? yyjson_read(monitors, strlen(monitors), 0) : NULL;
    bool covered = owe_outputs_covered_docs(c, m, fullscreen_only);
    yyjson_doc_free(c);
    yyjson_doc_free(m);
    return covered;
}
