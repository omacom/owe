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
    bool on_battery;
    bool locked;
    bool sleeping;
};

static int properties(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct owed_power *p = userdata;
    const char *iface;
    bool changed = false;
    (void)error;
    if (sd_bus_message_read(m, "s", &iface) < 0 || sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *key;
        if (sd_bus_message_read(m, "s", &key) < 0) return 0;
        bool *field = NULL;
        if (!strcmp(iface, "org.freedesktop.UPower") && !strcmp(key, "OnBattery")) field = &p->on_battery;
        if (!strcmp(iface, "org.freedesktop.login1.Session") && !strcmp(key, "LockedHint")) field = &p->locked;
        if (field) {
            int value;
            if (sd_bus_message_enter_container(m, 'v', "b") < 0 || sd_bus_message_read(m, "b", &value) < 0) return 0;
            sd_bus_message_exit_container(m);
            changed |= *field != (bool)value;
            *field = value;
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    if (changed) owed_app_on_policy_changed();
    return 0;
}

static int lock_signal(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct owed_power *p = userdata;
    (void)error;
    /* login1 session signals have no arguments. */
    p->locked = !strcmp(sd_bus_message_get_member(m), "Lock");
    owed_app_on_policy_changed();
    return 0;
}

static int sleep_signal(sd_bus_message *m, void *userdata, sd_bus_error *error) {
    struct owed_power *p = userdata;
    int value;
    (void)error;
    if (sd_bus_message_read(m, "b", &value) >= 0) {
        p->sleeping = value;
        owed_app_on_policy_changed();
    }
    return 0;
}

struct owed_power *owed_power_new(void) {
    struct owed_power *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    if (sd_bus_open_system(&p->system) < 0) return p;
    sd_bus_set_method_call_timeout(p->system, 500000);
    int value = 0;
    sd_bus_get_property_trivial(p->system, "org.freedesktop.UPower", "/org/freedesktop/UPower",
                               "org.freedesktop.UPower", "OnBattery", NULL, 'b', &value);
    p->on_battery = value;
    sd_bus_match_signal(p->system, NULL, "org.freedesktop.UPower", "/org/freedesktop/UPower",
                        "org.freedesktop.DBus.Properties", "PropertiesChanged", properties, p);
    sd_bus_match_signal(p->system, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager", "PrepareForSleep", sleep_signal, p);

    char *sid = NULL;
    /* A systemd user service often has no session in its own cgroup. */
    if (sd_pid_get_session(getpid(), &sid) < 0) sd_uid_get_display(getuid(), &sid);
    if (!sid && getenv("XDG_SESSION_ID")) sid = strdup(getenv("XDG_SESSION_ID"));
    sd_bus_message *reply = NULL;
    if (sid && sd_bus_call_method(p->system, "org.freedesktop.login1", "/org/freedesktop/login1",
                                 "org.freedesktop.login1.Manager", "GetSession", NULL, &reply, "s", sid) >= 0) {
        const char *path;
        if (sd_bus_message_read(reply, "o", &path) >= 0) {
            value = 0;
            sd_bus_get_property_trivial(p->system, "org.freedesktop.login1", path,
                                       "org.freedesktop.login1.Session", "LockedHint", NULL, 'b', &value);
            p->locked = value;
            sd_bus_match_signal(p->system, NULL, "org.freedesktop.login1", path,
                                "org.freedesktop.login1.Session", "Lock", lock_signal, p);
            sd_bus_match_signal(p->system, NULL, "org.freedesktop.login1", path,
                                "org.freedesktop.login1.Session", "Unlock", lock_signal, p);
            sd_bus_match_signal(p->system, NULL, "org.freedesktop.login1", path,
                                "org.freedesktop.DBus.Properties", "PropertiesChanged", properties, p);
        }
    }
    sd_bus_message_unref(reply);
    free(sid);
    return p;
}

void owed_power_free(struct owed_power *p) {
    if (!p) return;
    sd_bus_unref(p->system);
    free(p);
}

int owed_power_fd_system(struct owed_power *p) { return p && p->system ? sd_bus_get_fd(p->system) : -1; }
int owed_power_fd_session(struct owed_power *p) { (void)p; return -1; }
int owed_power_poll(struct owed_power *p) {
    if (p && p->system) while (sd_bus_process(p->system, NULL) > 0) {}
    return 0;
}
bool owed_power_on_battery(struct owed_power *p) { return p && p->on_battery; }
bool owed_power_locked(struct owed_power *p) { return p && p->locked; }
bool owed_power_sleeping(struct owed_power *p) { return p && p->sleeping; }
