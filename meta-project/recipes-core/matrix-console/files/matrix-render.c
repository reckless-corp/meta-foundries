/* SPDX-License-Identifier: BSD-2-Clause */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "matrix-fb.h"

static struct matrix_fb framebuffer = {.fd = -1, .tty = -1};
static void cleanup_fb(void) { fb_close(&framebuffer); }

static volatile sig_atomic_t stopping;
static const char characters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789$+-*/=%#&@<>:;|^~";

static void stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void *allocate(size_t count, size_t size)
{
    void *memory = calloc(count, size);
    if (!memory) {
        perror("matrix-render: allocation");
        exit(EXIT_FAILURE);
    }
    return memory;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/fb0", *console = "/dev/tty1";
    const char *artwork = NULL;
    unsigned scale = 1;
    int restore = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            puts("Usage: matrix-render "
                 "[--fb DEVICE] [--tty TTY] [--scale 1..4] ARTWORK\n"
                 "       matrix-render --restore-console [--tty TTY]\n"
                 "       matrix-render --font-license");
            return EXIT_SUCCESS;
        } else if (!strcmp(argv[i], "--font-license")) {
            puts(matrix_font_license);
            return EXIT_SUCCESS;
        } else if (!strcmp(argv[i], "--restore-console")) {
            restore = 1;
        } else if ((!strcmp(argv[i], "--fb") ||
                    !strcmp(argv[i], "--tty") || !strcmp(argv[i], "--scale")) && i + 1 < argc) {
            const char *option = argv[i++];
            if (!strcmp(option, "--fb")) device = argv[i];
            else if (!strcmp(option, "--tty")) console = argv[i];
            else {
                if (strlen(argv[i]) != 1 || argv[i][0] < '1' || argv[i][0] > '4')
                    goto usage;
                scale = (unsigned)(argv[i][0] - '0');
            }
        } else if (argv[i][0] == '-' || artwork) {
            goto usage;
        } else artwork = argv[i];
    }
    if (restore) {
        if (fb_restore_console(console) == 0) return EXIT_SUCCESS;
        perror("matrix-render: restore console");
        return EXIT_FAILURE;
    }
    if (!artwork) goto usage;
    atexit(cleanup_fb);
    FILE *file = fopen(artwork, "r");
    if (!file) {
        perror("matrix-render: artwork");
        return EXIT_FAILURE;
    }
    char **art = NULL, *line = NULL;
    size_t ah = 0, aw = 0, line_capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &line_capacity, file)) >= 0) {
        if (length && line[length - 1] == '\n')
            line[--length] = '\0';
        char **next = realloc(art, (ah + 1) * sizeof(*art));
        if (!next) {
            perror("matrix-render: artwork allocation");
            return EXIT_FAILURE;
        }
        art = next;
        art[ah] = allocate((size_t)length + 1, 1);
        memcpy(art[ah++], line, (size_t)length);
        if ((size_t)length > aw)
            aw = (size_t)length;
    }
    free(line);
    if (ferror(file)) {
        fprintf(stderr, "matrix-render: cannot read artwork\n");
        fclose(file);
        return EXIT_FAILURE;
    }
    fclose(file);

    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGPIPE, &action, NULL);
    if (fb_start(&framebuffer, device, console, scale) < 0)
        return EXIT_FAILURE;

    unsigned w = framebuffer.cols, h = framebuffer.rows, frames = 0;
    size_t cells = (size_t)w * h;
    char *rain = allocate(cells, 1), *heads = allocate(w, 1);
    char *overlay = allocate(cells, 1);
    char *previous_glyph = allocate(cells, 1), *previous_bright = allocate(cells, 1);
    unsigned *remaining = allocate(w, sizeof(*remaining));
    long ax = ((long)w - (long)aw) / 2;
    long ay = ((long)h - (long)ah) / 2;
    for (size_t r = 0; r < ah; ++r) {
        long y = ay + (long)r;
        if (y < 0 || y >= (long)h)
            continue;
        for (size_t c = 0; art[r][c]; ++c) {
            long x = ax + (long)c;
            if (x >= 0 && x < (long)w && art[r][c] != ' ')
                overlay[(size_t)y * w + (size_t)x] = art[r][c];
        }
    }
    for (size_t r = 0; r < ah; ++r)
        free(art[r]);
    free(art);

    int result = EXIT_SUCCESS;
    srand((unsigned)time(NULL));
    while (!stopping) {
        /* A changed layout requires a fresh mapping on service restart. */
        if (frames++ % 16 == 0 && framebuffer.active && !fb_layout_valid(&framebuffer)) {
            fprintf(stderr, "matrix-render: framebuffer layout changed; restart required\n");
            result = EXIT_FAILURE;
            break;
        }
        if (fb_events(&framebuffer, previous_glyph) < 0) {
            fprintf(stderr, "matrix-render: VT switch or framebuffer layout failure\n");
            result = EXIT_FAILURE;
            break;
        }
        memmove(rain + w, rain, (size_t)(h - 1) * w);
        memset(heads, 0, w);
        for (unsigned c = 0; c < w; ++c) {
            rain[c] = ' ';
            if (remaining[c]) {
                rain[c] = characters[rand() % (sizeof(characters) - 1)];
                --remaining[c];
            } else if ((double)rand() / ((double)RAND_MAX + 1) < 0.02) {
                rain[c] = characters[rand() % (sizeof(characters) - 1)];
                heads[c] = 1;
                remaining[c] = 4 + (unsigned)(rand() % 20);
            }
        }
        fb_render(&framebuffer, rain, heads, overlay, previous_glyph, previous_bright);
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 60000000};
        while (!stopping && nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
    }
    cleanup_fb();
    free(rain); free(heads); free(overlay); free(remaining);
    free(previous_glyph); free(previous_bright);
    return result;
usage:
    fprintf(stderr, "Usage: matrix-render "
            "[--fb DEVICE] [--tty TTY] [--scale 1..4] ARTWORK\n");
    return EXIT_FAILURE;
}
