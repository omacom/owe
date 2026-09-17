#include "policy.h"

#include <stdlib.h>
#include <string.h>

#include "daemon.h"
#include "hypr.h"
#include "log.h"
#include "power.h"

struct owed_policy {
    bool paused;
    bool poster;
    char reason[128];
    bool blocklisted;
    bool manual_pause;
    bool idle_pause;
};

struct owed_policy *owed_policy_new(void) {
    struct owed_policy *p = calloc(1, sizeof(*p));
    if (p) {
        snprintf(p->reason, sizeof(p->reason), "visible");
    }
    return p;
}

void owed_policy_free(struct owed_policy *p) {
    free(p);
}

void owed_policy_recompute(struct owed_policy *p) {
    owed_app_t *app;
    bool pause = false;
    bool poster = false;
    const char *reason = "visible";
    if (!p) {
        return;
    }
    app = owed_app_get();
    if (!app) {
        return;
    }
    if (p->manual_pause) {
        pause = true;
        reason = "manual";
        goto done;
    }
    if (owed_power_sleeping(app->power)) {
        pause = true;
        reason = "sleep";
        goto done;
    }
    if (app->always_animate) {
        reason = "always-animate";
        goto done;
    }
    if (p->idle_pause) {
        pause = true;
        reason = "idle";
        goto done;
    }
    if (app->power && owed_power_locked(app->power)) {
        pause = true;
        reason = "locked";
        goto done;
    }
    if (app->hypr && owed_hypr_all_monitors_off(app->hypr)) {
        pause = true;
        reason = "dpms-off";
        goto done;
    }
    if (app->power && owed_power_on_battery(app->power) && app->config.battery_poster) {
        pause = true;
        poster = true;
        reason = "battery";
        goto done;
    }
    if (p->blocklisted) {
        pause = true;
        reason = "blocklist";
        goto done;
    }
    if (app->hypr && app->config.pause_fullscreen && owed_hypr_any_fullscreen(app->hypr)) {
        pause = true;
        reason = "fullscreen";
        goto done;
    }
    if (app->hypr && app->config.pause_occupied_workspace &&
        owed_hypr_any_window_visible(app->hypr)) {
        pause = true;
        reason = "occupied";
        goto done;
    }
done:
    if (pause != p->paused || poster != p->poster || strcmp(reason, p->reason) != 0) {
        OWE_INFO("policy pause=%d poster=%d reason=%s", pause, poster, reason);
        owed_app_emit_event(pause ? "paused" : "resumed", reason);
    }
    p->paused = pause;
    p->poster = poster;
    snprintf(p->reason, sizeof(p->reason), "%s", reason);
}

bool owed_policy_should_pause(struct owed_policy *p) {
    return p && p->paused;
}

bool owed_policy_should_poster(struct owed_policy *p) {
    return p && p->poster;
}

const char *owed_policy_reason(struct owed_policy *p) {
    return p ? p->reason : "";
}

void owed_policy_set_blocklisted(struct owed_policy *p, bool blocked) {
    if (p && p->blocklisted != blocked) {
        p->blocklisted = blocked;
        owed_policy_recompute(p);
    }
}

void owed_policy_set_manual_pause(struct owed_policy *p, bool paused) {
    if (p) {
        p->manual_pause = paused;
    }
}

bool owed_policy_manual_pause(struct owed_policy *p) {
    return p && p->manual_pause;
}

void owed_policy_set_idle_pause(struct owed_policy *p, bool paused) {
    if (p) {
        p->idle_pause = paused;
    }
}

bool owed_policy_idle_pause(struct owed_policy *p) {
    return p && p->idle_pause;
}
