#pragma once

/* `owe-render --scene-frame`: draw moments of a scene to picture files, with no
 * window and no daemon. Returns the process's exit status. */
int owe_scene_frame_main(int argc, char **argv);

/* `owe-render --scene-info`: read a scene file as the player would and say, as
 * JSON, what it holds or why it will not play. Returns the exit status. */
int owe_scene_info_main(int argc, char **argv);
