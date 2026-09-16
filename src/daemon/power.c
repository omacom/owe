#include "power.h"

#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <unistd.h>

#include "daemon.h"
#include "log.h"

struct owed_power {
    sd_bus *system;
    sd_bus *session;
    bool on_battery;
    bool locked;
    char session_id[64];
};

static int upower_get_on_battery(sd_bus *bus, bool *out) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int b = 0;
    int rc;
    rc = sd_bus_call_method(bus, "org.freedesktop.UPower", "/org/freedesktop/UPower",
                            "org.freedesktop.DBus.Properties", "Get", &error, &reply, "ss",
                            "org.freedesktop.UPower", "OnBattery");
    if (rc < 0) {
        sd_bus_error_free(&error);
        return -1;
    }
    rc = sd_bus_message_read(reply, "v", "b", &b);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    if (rc < 0) {
        return -1;
    }
    *out = b ? true : false;
    return 0;
}

static int match_upower(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct owed_power *p = userdata;
    const char *iface;
    const char *member;
    (void)error;
    iface = sd_bus_message_get_interface(m);
    member = sd_bus_message_get_member(m);
    if (iface && member && strcmp(iface, "org.freedesktop.DBus.Properties") == 0 &&
        strcmp(member, "PropertiesChanged") == 0) {
        bool nb = p->on_battery;
        if (upower_get_on_battery(p->system, &nb) == 0 && nb != p->on_battery) {
            p->on_battery = nb;
            OWE_INFO("on_battery=%d", nb);
            owed_app_on_policy_changed();
        }
    }
    return 0;
}

static int match_lock(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct owed_power *p = userdata;
    const char *member;
    const char *sid = NULL;
    (void)error;
    member = sd_bus_message_get_member(m);
    if (!member) {
        return 0;
    }
    if (strcmp(member, "Lock") == 0 || strcmp(member, "Unlock") == 0) {
        sd_bus_message_read(m, "s", &sid);
        if (sid && p->session_id[0] && strcmp(sid, p->session_id) != 0) {
            return 0;
        }
        p->locked = strcmp(member, "Lock") == 0;
        OWE_INFO("locked=%d", p->locked);
        owed_app_on_policy_changed();
    }
    return 0;
}

static int match_sleep(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct owed_power *p = userdata;
    int going = 0;
    (void)p;
    (void)error;
    if (strcmp(sd_bus_message_get_member(m), "PrepareForSleep") != 0) {
        return 0;
    }
    sd_bus_message_read(m, "b", &going);
    OWE_INFO("prepare_for_sleep=%d", going);
    owed_app_on_policy_changed();
    return 0;
}

struct owed_power *owed_power_new(void) {
    struct owed_power *p = calloc(1, sizeof(*p));
    char *sid = NULL;
    if (!p) {
        return NULL;
    }
    if (sd_bus_open_system(&p->system) < 0) {
        OWE_WARN("system bus unavailable");
    }
    if (sd_bus_open_user(&p->session) < 0) {
        OWE_WARN("session bus unavailable");
    }
    if (sd_pid_get_session(getpid(), &sid) >= 0 && sid) {
        snprintf(p->session_id, sizeof(p->session_id), "%s", sid);
        free(sid);
    }
    if (p->system) {
        upower_get_on_battery(p->system, &p->on_battery);
        sd_bus_match_signal(p->system, NULL, "org.freedesktop.UPower", "/org/freedesktop/UPower",
                            "org.freedesktop.DBus.Properties", "PropertiesChanged", match_upower, p);
        sd_bus_match_signal(p->system, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager", "PrepareForSleep", match_sleep, p);
    }
    if (p->session) {
        sd_bus_match_signal(p->session, NULL, "org.freedesktop.login1",
                            "/org/freedesktop/login1/session/self", "org.freedesktop.login1.Session",
                            "Lock", match_lock, p);
        sd_bus_match_signal(p->session, NULL, "org.freedesktop.login1",
                            "/org/freedesktop/login1/session/self", "org.freedesktop.login1.Session",
                            "Unlock", match_lock, p);
    }
    OWE_INFO("power ready battery=%d locked=%d", p->on_battery, p->locked);
    return p;
}

void owed_power_free(struct owed_power *p) {
    if (!p) {
        return;
    }
    if (p->system) {
        sd_bus_unref(p->system);
    }
    if (p->session) {
        sd_bus_unref(p->session);
    }
    free(p);
}

int owed_power_fd_system(struct owed_power *p) {
    if (!p || !p->system) {
        return -1;
    }
    return sd_bus_get_fd(p->system);
}

int owed_power_fd_session(struct owed_power *p) {
    if (!p || !p->session) {
        return -1;
    }
    return sd_bus_get_fd(p->session);
}

int owed_power_poll(struct owed_power *p) {
    if (!p) {
        return 0;
    }
    if (p->system) {
        while (sd_bus_process(p->system, NULL) > 0) {
        }
    }
    if (p->session) {
        while (sd_bus_process(p->session, NULL) > 0) {
        }
    }
    return 0;
}

bool owed_power_on_battery(struct owed_power *p) {
    return p && p->on_battery;
}

bool owed_power_locked(struct owed_power *p) {
    return p && p->locked;
}
