#pragma once

#include <stdbool.h>

struct owed_policy;

struct owed_policy *owed_policy_new(void);
void owed_policy_free(struct owed_policy *p);

void owed_policy_recompute(struct owed_policy *p);
bool owed_policy_should_pause(struct owed_policy *p);
bool owed_policy_should_poster(struct owed_policy *p);
const char *owed_policy_reason(struct owed_policy *p);

void owed_policy_set_blocklisted(struct owed_policy *p, bool blocked);
void owed_policy_set_manual_pause(struct owed_policy *p, bool paused);
bool owed_policy_manual_pause(struct owed_policy *p);
void owed_policy_set_idle_pause(struct owed_policy *p, bool paused);
bool owed_policy_idle_pause(struct owed_policy *p);
