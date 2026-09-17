#pragma once

struct owed_supervisor;

struct owed_supervisor *owed_supervisor_new(void);
void owed_supervisor_free(struct owed_supervisor *s);

int owed_supervisor_ensure_running(struct owed_supervisor *s);
void owed_supervisor_reap(struct owed_supervisor *s);
void owed_supervisor_stop(struct owed_supervisor *s);

int owed_supervisor_send(struct owed_supervisor *s, const char *line, char *reply,
                         unsigned long reply_len);
int owed_supervisor_load(struct owed_supervisor *s, const char *path, const char *kind);
int owed_supervisor_pause(struct owed_supervisor *s);
int owed_supervisor_resume(struct owed_supervisor *s);
int owed_supervisor_stop_render(struct owed_supervisor *s);
int owed_supervisor_fade(struct owed_supervisor *s, int ms);
int owed_render_is_alive(struct owed_supervisor *s);
