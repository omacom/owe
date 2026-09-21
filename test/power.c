#include "../src/daemon/power.c"
#include <errno.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static int attempts, closes;
int sd_bus_open_system(sd_bus **bus) { *bus = NULL; attempts++; return -ECONNREFUSED; }
int sd_bus_process(sd_bus *bus, sd_bus_message **message) {
    (void)bus; (void)message; return -ECONNRESET;
}
sd_bus *sd_bus_close_unref(sd_bus *bus) { (void)bus; closes++; return NULL; }
void owed_app_on_policy_changed(void) {}

int main(void) {
    struct owed_power p = {.system = (sd_bus *)(uintptr_t)1};
    CHECK(owed_power_poll(&p) == -1);
    CHECK(closes == 1 && !p.system && owed_power_fd_system(&p) == -1);
    CHECK(owed_power_poll(&p) == 0 && attempts == 0);
    p.retry_at_ms = 0;
    CHECK(owed_power_poll(&p) == 0 && attempts == 1);
    CHECK(owed_power_poll(&p) == 0 && attempts == 1);
    puts("disconnected system bus is removed from poll and reconnects with backoff");
}
