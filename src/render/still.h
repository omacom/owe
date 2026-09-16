#pragma once

#include <stdbool.h>

struct owe_still;
struct owe_wayland;
struct owe_egl;
struct owe_output;

struct owe_still *owe_still_new(struct owe_wayland *wl, struct owe_egl *egl);
void owe_still_free(struct owe_still *s);

int owe_still_load(struct owe_still *s, const char *path);
void owe_still_unload(struct owe_still *s);
bool owe_still_has_image(struct owe_still *s);
const char *owe_still_path(struct owe_still *s);

void owe_still_render_output(struct owe_still *s, struct owe_output *out);
void owe_still_set_fade_ms(struct owe_still *s, int ms);
bool owe_still_needs_frames(struct owe_still *s);
