#pragma once

#include <stdbool.h>

struct owed_power;

struct owed_power *owed_power_new(void);
void owed_power_free(struct owed_power *p);
int owed_power_fd_system(struct owed_power *p);
int owed_power_poll(struct owed_power *p);

bool owed_power_on_battery(struct owed_power *p);
bool owed_power_locked(struct owed_power *p);
bool owed_power_sleeping(struct owed_power *p);
