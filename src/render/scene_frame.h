#pragma once

/* `owe-render --scene-frame`: draw moments of a scene to picture files, with no
 * window and no daemon. Returns the process's exit status. */
int owe_scene_frame_main(int argc, char **argv);
