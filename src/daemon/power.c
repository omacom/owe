#include "power.h"
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <unistd.h>
#include "daemon.h"
#include "log.h"

struct owed_power {
    sd_bus *system;
    int64_t retry_at_ms;
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

static int64_t power_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void connect_system(struct owed_power *p) {
    p->retry_at_ms = power_now_ms() + 30000;
    if (sd_bus_open_system(&p->system) < 0) return;
    sd_bus_set_method_call_timeout(p->system, 500000);
    int value = 0;
    if (sd_bus_get_property_trivial(p->system, "org.freedesktop.UPower", "/org/freedesktop/UPower",
                                   "org.freedesktop.UPower", "OnBattery", NULL, 'b', &value) >= 0)
        p->on_battery = value;
    if (sd_bus_get_property_trivial(p->system, "org.freedesktop.login1", "/org/freedesktop/login1",
                                   "org.freedesktop.login1.Manager", "PreparingForSleep", NULL, 'b', &value) >= 0)
        p->sleeping = value;
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
            if (sd_bus_get_property_trivial(p->system, "org.freedesktop.login1", path,
                                           "org.freedesktop.login1.Session", "LockedHint", NULL, 'b', &value) >= 0)
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
}

struct owed_power *owed_power_new(void) {
    struct owed_power *p = calloc(1, sizeof(*p));
    if (p) connect_system(p);
    return p;
}

void owed_power_free(struct owed_power *p) {
    if (!p) return;
    sd_bus_unref(p->system);
    free(p);
}

int owed_power_fd_system(struct owed_power *p) { return p && p->system ? sd_bus_get_fd(p->system) : -1; }
int owed_power_poll(struct owed_power *p) {
    if (!p) return 0;
    if (!p->system && power_now_ms() >= p->retry_at_ms) {
        connect_system(p);
        if (p->system) owed_app_on_policy_changed();
    }
    if (p->system) {
        int rc;
        while ((rc = sd_bus_process(p->system, NULL)) > 0) {}
        if (rc < 0) {
            OWE_WARN("system bus disconnected; retrying in 30 seconds");
            p->system = sd_bus_close_unref(p->system);
            p->retry_at_ms = power_now_ms() + 30000;
            return -1;
        }
    }
    return 0;
}
bool owed_power_on_battery(struct owed_power *p) { return p && p->on_battery; }
bool owed_power_locked(struct owed_power *p) { return p && p->locked; }
bool owed_power_sleeping(struct owed_power *p) { return p && p->sleeping; }
