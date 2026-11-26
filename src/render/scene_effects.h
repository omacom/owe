#pragma once

#include "scene_desc.h"

/* The effects OWE has. An effect is a small shader that redraws an area of
 * the still: it is given a place in the picture and says what colour is there
 * now. A scene can bring its own, written the same way; these are also the
 * examples to start from. */
struct owe_scene_builtin {
    const char *name;
    const char *source;
    float seconds; /* one cycle, unless the scene says otherwise */
    struct owe_scene_value values[OWE_SCENE_MAX_VALUES];
    int value_count;
};

const struct owe_scene_builtin *owe_scene_builtin(const char *name);
/* The i-th of them, or NULL past the last. */
const struct owe_scene_builtin *owe_scene_builtin_at(int i);
