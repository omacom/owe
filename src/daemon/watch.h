#pragma once

struct owed_watch;

struct owed_watch *owed_watch_new(void);
void owed_watch_free(struct owed_watch *w);
int owed_watch_fd(struct owed_watch *w);
int owed_watch_poll(struct owed_watch *w);

int owed_watch_resolve_current(char *buf, unsigned long len);
