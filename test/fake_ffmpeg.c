#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    const char *mode = getenv("OWE_TEST_FFMPEG_MODE");
    FILE *out = fopen(argv[argc - 1], "w");
    if (!out) return 2;
    fputs("partial", out);
    fflush(out);
    const char *started = getenv("OWE_TEST_FFMPEG_STARTED");
    FILE *marker = started ? fopen(started, "w") : NULL;
    if (marker) { fprintf(marker, "%d\n", getpid()); fclose(marker); }
    if (mode && strcmp(mode, "wait") == 0) {
        for (;;) pause();
    }
    usleep(200000);
    fputs(" complete", out);
    fclose(out);
    return mode && strcmp(mode, "fail") == 0 ? 3 : 0;
}
