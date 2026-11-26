#include "../src/render/scene_desc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-4)

static char dir[] = "/tmp/owe-scene-desc-XXXXXX";
static char file[4200];

static int read_text(const char *text, struct owe_scene_desc *desc, char *error, size_t len) {
    FILE *f = fopen(file, "w");
    CHECK(f);
    fputs(text, f);
    fclose(f);
    return owe_scene_desc_read(file, desc, error, len);
}

int main(void) {
    /* Too large for the stack of some threads, and zeroed by the reader. */
    static struct owe_scene_desc desc;
    char error[256] = "";
    char expect[4300];
    CHECK(mkdtemp(dir));
    snprintf(file, sizeof(file), "%s/scene.jsonc", dir);

    /* A whole scene, with comments and a trailing comma, as people write them. */
    CHECK(read_text("// a scene\n"
                    "{\n"
                    "  \"still\": \"theme/still.webp\",\n"
                    "  \"sha256\": \"4896da620ed3016b7e52eede8db8169591a9fa4856064db4039521be96f724b6\",\n"
                    "  \"areas\": [ { \"name\": \"eyes\", \"mask\": \"pieces/eyes.png\", \"at\": [0.4, 0.5, 0.1, 0.05] } ],\n"
                    "  \"pieces\": [\n"
                    "    { \"name\": \"disc\", \"picture\": \"pieces/disc.png\", \"behind\": \"pieces/disc-behind.png\", \"at\": [0.3, 0.2, 0.3, 0.6], \"colour\": \"own\" },\n"
                    "    { \"name\": \"sprite\", \"picture\": \"pieces/sprite.png\", \"behind\": \"pieces/sprite-behind.png\", \"at\": [0.5, 0.3, 0.03, 0.05], \"on\": \"disc\" },\n"
                    "    { \"name\": \"lids\", \"picture\": \"pieces/lids.png\", \"at\": [0.4, 0.5, 0.1, 0.05], \"inside\": \"eyes\", \"rest\": { \"y\": -0.04 } },\n"
                    "  ],\n"
                    "  \"idle\": {\n"
                    "    \"frames_a_second\": 12,\n"
                    "    \"sprite\": { \"y\": { \"swing\": 0.005, \"seconds\": 8, \"phase\": 90 } },\n"
                    "    \"lids\": { \"y\": { \"pulse\": 0.04, \"at\": [1, 4], \"lasts\": 0.3, \"every\": 8 } }\n"
                    "  },\n"
                    "  \"arrive\": {\n"
                    "    \"seconds\": 5,\n"
                    "    \"disc\": { \"size\": { \"from\": 0, \"after\": 0.25, \"over\": 0.2, \"peak\": 0.4 } },\n"
                    "    \"sprite\": { \"fade\": { \"from\": 0, \"after\": 2, \"for\": 0.5 } }\n"
                    "  }\n"
                    "}\n",
                    &desc, error, sizeof(error)) == 0);
    CHECK(strcmp(desc.still, "theme/still.webp") == 0 && strlen(desc.sha256) == 64);
    CHECK(desc.area_count == 1 && desc.piece_count == 3);
    CHECK(desc.idle_fps == 12 && desc.arrive_fps == 30 && NEAR(desc.arrive_seconds, 5));
    snprintf(expect, sizeof(expect), "%s/pieces/sprite.png", dir);
    CHECK(strcmp(desc.pieces[1].picture, expect) == 0);
    CHECK(desc.pieces[0].own_colour && !desc.pieces[1].own_colour);
    CHECK(desc.pieces[1].on == 0 && desc.pieces[2].inside == 0 && desc.pieces[0].on == -1);
    CHECK(!*desc.pieces[2].behind && NEAR(desc.pieces[2].rest_y, -0.04));
    /* A part lists only the pieces it moves. */
    CHECK(!desc.pieces[0].idle.listed && desc.pieces[1].idle.listed && desc.pieces[0].arrive.listed && !desc.pieces[2].arrive.listed);

    /* A swing starts at home wherever in its cycle it starts, and comes home each cycle. */
    const struct owe_scene_motion *swing = &desc.pieces[1].idle.y;
    CHECK(NEAR(owe_scene_offset(swing, 0), 0) && NEAR(owe_scene_offset(swing, 8), 0));
    CHECK(NEAR(owe_scene_offset(swing, 4), -0.01));
    /* A pulse is at rest between its moments and fully out in the middle of one. */
    const struct owe_scene_motion *pulse = &desc.pieces[2].idle.y;
    CHECK(NEAR(owe_scene_offset(pulse, 0.5), 0) && NEAR(owe_scene_offset(pulse, 1.15), 0.04) && NEAR(owe_scene_offset(pulse, 12.15), 0.04));
    /* A size that swings past rest: nothing before it sets off, furthest at its peak, then settled. */
    const struct owe_scene_motion *grow = &desc.pieces[0].arrive.size;
    CHECK(NEAR(owe_scene_level(grow, 0.2), 0) && NEAR(owe_scene_level(grow, 0.65), 1.2) && NEAR(owe_scene_level(grow, 5), 1));
    /* A fade that eases in over its time and never passes rest. */
    const struct owe_scene_motion *fade = &desc.pieces[1].arrive.fade;
    CHECK(NEAR(owe_scene_level(fade, 2), 0) && owe_scene_level(fade, 2.25) > 0.5f && owe_scene_level(fade, 2.25) < 1 && NEAR(owe_scene_level(fade, 2.5), 1));
    /* A value with no motion stays at rest. */
    CHECK(NEAR(owe_scene_offset(&desc.pieces[1].idle.x, 3), 0) && NEAR(owe_scene_level(&desc.pieces[1].idle.size, 3), 1));

    /* A scene may only read files of its own. */
    CHECK(read_text("{\"still\":\"a/b.png\",\"pieces\":[{\"name\":\"p\",\"picture\":\"/etc/passwd\",\"at\":[0,0,1,1]}]}", &desc, error, sizeof(error)) != 0);
    CHECK(strstr(error, "of the scene's own"));
    CHECK(read_text("{\"still\":\"a/b.png\",\"pieces\":[{\"name\":\"p\",\"picture\":\"../../secret.png\",\"at\":[0,0,1,1]}]}", &desc, error, sizeof(error)) != 0);
    CHECK(read_text("{\"still\":\"a/b.png\",\"pieces\":[{\"name\":\"p\",\"picture\":\"a..b/p.png\",\"at\":[0,0,1,1]}]}", &desc, error, sizeof(error)) == 0);
    /* A part that moves a piece the scene does not have is a mistake worth naming. */
    CHECK(read_text("{\"still\":\"a/b.png\",\"pieces\":[{\"name\":\"p\",\"picture\":\"p.png\",\"at\":[0,0,1,1]}],\"idle\":{\"q\":{}}}", &desc, error, sizeof(error)) != 0);
    CHECK(strstr(error, "\"q\""));
    /* A place outside the still, a piece on one that comes after it, a frame rate nobody can play. */
    CHECK(read_text("{\"still\":\"a/b.png\",\"pieces\":[{\"name\":\"p\",\"picture\":\"p.png\",\"at\":[0.9,0,0.2,1]}]}", &desc, error, sizeof(error)) != 0);
    CHECK(read_text("{\"still\":\"a/b.png\",\"pieces\":[{\"name\":\"p\",\"picture\":\"p.png\",\"at\":[0,0,1,1],\"on\":\"q\"},{\"name\":\"q\",\"picture\":\"q.png\",\"at\":[0,0,1,1]}]}", &desc, error, sizeof(error)) != 0);
    CHECK(read_text("{\"still\":\"a/b.png\",\"idle\":{\"frames_a_second\":240}}", &desc, error, sizeof(error)) != 0);
    CHECK(read_text("{\"pieces\":[]}", &desc, error, sizeof(error)) != 0);
    CHECK(read_text("not json", &desc, error, sizeof(error)) != 0);

    /* Effects work on areas: one of OWE's by its name, with OWE's values unless the part says otherwise,
     * and one of the scene's own, a shader file with its values. */
    CHECK(read_text("{\"still\":\"a/b.png\",\"areas\":[{\"name\":\"sky\",\"mask\":\"sky.png\",\"at\":[0,0,1,0.5]}],"
                    "\"effects\":[{\"name\":\"haze\",\"shader\":\"fx/haze.glsl\",\"values\":{\"seconds\":20,\"tint\":[1,0.5,0]}}],"
                    "\"idle\":{\"sky\":{\"twinkle\":{\"amount\":0.2,\"seconds\":4},\"haze\":{}}},"
                    "\"arrive\":{\"seconds\":3,\"sky\":{\"haze\":{\"strength\":{\"from\":0,\"after\":1,\"for\":1}}}}}",
                    &desc, error, sizeof(error)) == 0);
    CHECK(desc.effect_count == 2 && strcmp(desc.effects[0].name, "haze") == 0 && strstr(desc.effects[0].shader, "/fx/haze.glsl"));
    CHECK(NEAR(desc.effects[0].seconds, 20) && desc.effects[0].value_count == 1 && desc.effects[0].values[0].count == 3);
    CHECK(strcmp(desc.effects[1].name, "twinkle") == 0 && !*desc.effects[1].shader && desc.effects[1].value_count == 2);
    CHECK(desc.idle_use_count == 2 && desc.idle_uses[0].effect == 1 && desc.idle_uses[0].area == 0 && NEAR(desc.idle_uses[0].seconds, 4));
    CHECK(desc.idle_uses[0].value_count == 1 && strcmp(desc.idle_uses[0].values[0].name, "amount") == 0 && NEAR(desc.idle_uses[0].values[0].v[0], 0.2));
    CHECK(NEAR(desc.idle_uses[1].seconds, 20) && NEAR(owe_scene_level(&desc.idle_uses[1].strength, 0), 1));
    CHECK(desc.arrive_use_count == 1 && NEAR(owe_scene_level(&desc.arrive_uses[0].strength, 0.5), 0) && NEAR(owe_scene_level(&desc.arrive_uses[0].strength, 2.5), 1));
    /* An effect nobody has, a shader outside the scene, a value whose name would break a shader. */
    CHECK(read_text("{\"still\":\"a/b.png\",\"areas\":[{\"name\":\"sky\",\"mask\":\"sky.png\",\"at\":[0,0,1,1]}],\"idle\":{\"sky\":{\"sparkle\":{}}}}", &desc, error, sizeof(error)) != 0);
    CHECK(strstr(error, "\"sparkle\""));
    CHECK(read_text("{\"still\":\"a/b.png\",\"effects\":[{\"name\":\"haze\",\"shader\":\"../haze.glsl\"}]}", &desc, error, sizeof(error)) != 0);
    CHECK(read_text("{\"still\":\"a/b.png\",\"areas\":[{\"name\":\"sky\",\"mask\":\"sky.png\",\"at\":[0,0,1,1]}],\"idle\":{\"sky\":{\"glow\":{\"a; discard\":1}}}}", &desc, error, sizeof(error)) != 0);

    unlink(file);
    rmdir(dir);
    puts("scene description passed");
    return 0;
}
