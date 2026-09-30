/* A small fbdev experiment: query -> mmap -> draw pixels -> restore.
 * No window system, GPU API, external font, or display mode change is needed.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

struct canvas {
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    unsigned char *pixels;
    unsigned bytes_per_pixel;
};

static volatile sig_atomic_t interrupted;

static void on_signal(int number)
{
    (void)number;
    interrupted = 1; /* Cleanup uses normal control flow, not a signal handler. */
}

static int validate(struct canvas *c)
{
    const struct fb_var_screeninfo *v = &c->var;
    const uint32_t endian = 1;
    if (*(const unsigned char *)&endian != 1 ||
        c->fix.type != FB_TYPE_PACKED_PIXELS || c->fix.visual != FB_VISUAL_TRUECOLOR ||
        v->grayscale || v->nonstd || v->rotate ||
        (v->bits_per_pixel != 16 && v->bits_per_pixel != 24 && v->bits_per_pixel != 32))
        return -1;
    c->bytes_per_pixel = v->bits_per_pixel / 8;
    const struct fb_bitfield fields[] = {v->red, v->green, v->blue, v->transp};
    uint32_t used = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned bits = fields[i].length, shift = fields[i].offset;
        if (i == 3 && bits == 0)
            continue;
        if (!bits || bits > 8 || fields[i].msb_right || shift >= v->bits_per_pixel ||
            bits > v->bits_per_pixel - shift)
            return -1;
        uint32_t mask = ((1u << bits) - 1) << shift;
        if (used & mask)
            return -1;
        used |= mask;
    }
    /* Use wide arithmetic before checking offsets, stride and mapping bounds. */
    uint64_t right = (uint64_t)v->xoffset + v->xres;
    uint64_t bottom = (uint64_t)v->yoffset + v->yres;
    uint64_t row_end = right * c->bytes_per_pixel;
    if (!v->xres || !v->yres || right > v->xres_virtual || bottom > v->yres_virtual ||
        row_end > c->fix.line_length ||
        (bottom - 1) * c->fix.line_length + row_end > c->fix.smem_len)
        return -1;
    return 0;
}

static uint32_t channel(unsigned value, struct fb_bitfield field)
{
    if (!field.length)
        return 0;
    return ((value * ((1u << field.length) - 1) + 127) / 255) << field.offset;
}

static uint32_t rgb(const struct canvas *c, unsigned r, unsigned g, unsigned b)
{
    return channel(r, c->var.red) | channel(g, c->var.green) |
           channel(b, c->var.blue) | channel(255, c->var.transp);
}

static unsigned char *row_address(const struct canvas *c, unsigned y)
{
    return c->pixels + ((size_t)y + c->var.yoffset) * c->fix.line_length +
           (size_t)c->var.xoffset * c->bytes_per_pixel;
}

static void rectangle(const struct canvas *c, unsigned x, unsigned y,
                      unsigned width, unsigned height, uint32_t color)
{
    if (x >= c->var.xres || y >= c->var.yres)
        return;
    if (width > c->var.xres - x) width = c->var.xres - x;
    if (height > c->var.yres - y) height = c->var.yres - y;
    for (unsigned r = 0; r < height; ++r) {
        unsigned char *pixel = row_address(c, y + r) + (size_t)x * c->bytes_per_pixel;
        for (unsigned col = 0; col < width; ++col) {
            /* memcpy also works for unaligned 16/24/32-bit pixel addresses. */
            memcpy(pixel, &color, c->bytes_per_pixel);
            pixel += c->bytes_per_pixel;
        }
    }
}

/* Hand-drawn 5x7 letters. Each byte is one row; bit 4 is the leftmost pixel.
 * For example, F uses 11111 for its top bar and 10000 for its vertical stem. */
static const struct { char letter; unsigned char rows[7]; } font[] = {
    {'F', {31,16,16,30,16,16,16}}, {'B', {30,17,17,30,17,17,30}},
    {'D', {30,17,17,17,17,17,30}}, {'E', {31,16,16,30,16,16,31}},
    {'M', {17,27,21,21,17,17,17}}, {'O', {14,17,17,17,17,17,14}},
    {'R', {30,17,17,30,20,18,17}}, {'G', {14,17,16,23,17,17,14}},
    {'0', {14,17,19,21,25,17,14}}, {'1', {4,12,4,4,4,4,14}},
    {'2', {14,17,1,2,4,8,31}}, {'3', {30,1,1,14,1,1,30}},
};

static void text(const struct canvas *c, unsigned x, unsigned y, unsigned scale,
                 const char *message, uint32_t color)
{
    for (; *message; ++message, x += 6 * scale) {
        for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); ++i) {
            if (font[i].letter != *message) continue;
            for (unsigned row = 0; row < 7; ++row)
                for (unsigned col = 0; col < 5; ++col)
                    if (font[i].rows[row] & (1u << (4 - col)))
                        rectangle(c, x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void draw_demo(const struct canvas *c)
{
    unsigned w = c->var.xres, h = c->var.yres;
    uint32_t white = rgb(c, 255, 255, 255);
    rectangle(c, 0, 0, w, h, rgb(c, 0, 0, 0));
    /* Four bars verify color packing before we try drawing letters. */
    const uint32_t colors[] = {rgb(c,255,0,0), rgb(c,0,255,0), rgb(c,0,0,255), white};
    for (unsigned i = 0; i < 4; ++i) {
        unsigned left = (unsigned)((uint64_t)w * i / 4);
        unsigned right = (unsigned)((uint64_t)w * (i + 1) / 4);
        rectangle(c, left, 0, right - left, h / 3, colors[i]);
    }
    unsigned scale = w / 60;
    if (scale > h / 30) scale = h / 30;
    if (scale > 12) scale = 12;
    if (!scale) scale = 1;
    text(c, scale, h / 2, scale, "FB DEMO", rgb(c, 0, 255, 0));
    text(c, scale, h / 2 + 9 * scale, scale, "RGB 0123", white);
}

static void describe(const struct canvas *c)
{
    printf("Framebuffer: %.16s\nVisible: %ux%u; virtual: %ux%u; offset: %u,%u\n"
           "Pixel depth: %u; stride: %u bytes; mapping: %u bytes\n"
           "RGB bit offset/length: %u/%u %u/%u %u/%u; alpha: %u/%u\n",
           c->fix.id, c->var.xres, c->var.yres, c->var.xres_virtual, c->var.yres_virtual,
           c->var.xoffset, c->var.yoffset, c->var.bits_per_pixel, c->fix.line_length,
           c->fix.smem_len, c->var.red.offset, c->var.red.length, c->var.green.offset,
           c->var.green.length, c->var.blue.offset, c->var.blue.length,
           c->var.transp.offset, c->var.transp.length);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *fb_path = "/dev/fb0", *tty_path = "/dev/tty1";
    int info = 0, restore = 0;
    unsigned seconds = 10;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--info")) info = 1;
        else if (!strcmp(argv[i], "--restore-text")) restore = 1;
        else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fb_path = argv[++i];
        else if (!strcmp(argv[i], "--tty") && i + 1 < argc) tty_path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            char *end;
            errno = 0;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (errno || *end || !value || value > 3600) {
                fprintf(stderr, "--seconds must be between 1 and 3600\n");
                return 1;
            }
            seconds = (unsigned)value;
        } else {
            fprintf(stderr, "Usage: %s [--info | --restore-text] [--fb DEVICE] "
                    "[--tty DEVICE] [--seconds 1..3600]\n", argv[0]);
            return strcmp(argv[i], "--help") ? 1 : 0;
        }
    }
    if (info && restore) {
        fprintf(stderr, "Use either --info or --restore-text, not both.\n");
        return 1;
    }
    if (restore) {
        int fd = open(tty_path, O_RDWR | O_NOCTTY);
        if (fd < 0) { perror(tty_path); return 1; }
        int result = ioctl(fd, KDSETMODE, KD_TEXT);
        if (result < 0) perror("restore text mode");
        close(fd);
        return result < 0;
    }
    struct canvas c = {0};
    int fb = open(fb_path, info ? O_RDONLY : O_RDWR);
    if (fb < 0) { perror(fb_path); return 1; }
    int result = 1, tty = -1, graphics = 0, old_mode = KD_TEXT;
    unsigned char *saved = NULL;
    if (ioctl(fb, FBIOGET_FSCREENINFO, &c.fix) < 0 ||
        ioctl(fb, FBIOGET_VSCREENINFO, &c.var) < 0) {
        perror("framebuffer information");
        goto cleanup;
    }
    describe(&c);
    if (validate(&c) < 0) {
        fprintf(stderr, "Unsupported pixel format or invalid framebuffer bounds. "
                "This demo supports little-endian packed truecolor RGB at 16/24/32 bits.\n");
        goto cleanup;
    }
    if (info) { result = 0; goto cleanup; }
    tty = open(tty_path, O_RDWR | O_NOCTTY);
    if (tty < 0) { perror(tty_path); goto cleanup; }
    struct stat st;
    struct vt_stat vt;
    if (fstat(tty, &st) < 0 || ioctl(tty, VT_GETSTATE, &vt) < 0 ||
        ioctl(tty, KDGETMODE, &old_mode) < 0) {
        perror("virtual console information");
        goto cleanup;
    }
    if (major(st.st_rdev) != 4 || minor(st.st_rdev) != vt.v_active || old_mode != KD_TEXT) {
        fprintf(stderr, "Use the active virtual console, currently in text mode.\n");
        goto cleanup;
    }
    c.pixels = mmap(NULL, c.fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (c.pixels == MAP_FAILED) { c.pixels = NULL; perror("mmap framebuffer"); goto cleanup; }
    size_t row_bytes = (size_t)c.var.xres * c.bytes_per_pixel;
    saved = malloc(row_bytes * c.var.yres);
    if (!saved) { perror("save framebuffer"); goto cleanup; }
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGPIPE, &action, NULL);
    if (interrupted) goto cleanup;
    if (ioctl(tty, KDSETMODE, KD_GRAPHICS) < 0) { perror("enter graphics mode"); goto cleanup; }
    graphics = 1;
    for (unsigned y = 0; y < c.var.yres; ++y)
        memcpy(saved + y * row_bytes, row_address(&c, y), row_bytes);
    printf("Drawing for %u seconds; Ctrl+C or SIGTERM restores text mode.\n", seconds);
    fflush(stdout);
    if (!interrupted) draw_demo(&c);
    struct timespec delay = {.tv_sec = seconds};
    while (!interrupted && nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    result = 0;
cleanup:
    if (graphics) {
        size_t bytes = (size_t)c.var.xres * c.bytes_per_pixel;
        for (unsigned y = 0; y < c.var.yres; ++y)
            memcpy(row_address(&c, y), saved + y * bytes, bytes);
        if (ioctl(tty, KDSETMODE, old_mode) < 0) {
            perror("restore console mode (use --restore-text to retry)");
            result = 1;
        }
    }
    free(saved);
    if (c.pixels) munmap(c.pixels, c.fix.smem_len);
    if (tty >= 0) close(tty);
    close(fb);
    return result;
}
