/* SPDX-License-Identifier: BSD-2-Clause */
/* fbdev backend: validated pixel layout and rectangle drawing come from fb-demo.
 * Tiles are built once in normal RAM; the hot path only copies changed tiles.
 */
#ifndef MATRIX_FB_H
#define MATRIX_FB_H
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

struct canvas {
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    unsigned char *pixels;
    unsigned bytes_per_pixel;
};

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

#include "matrix-font.h"

struct matrix_fb {
    struct canvas canvas;
    int fd, tty, graphics, managed, active;
    struct vt_mode old_vt;
    unsigned width, height, cols, rows;
    size_t tile_bytes;
    unsigned char *tiles;
};

static volatile sig_atomic_t fb_release, fb_acquire;
static void fb_vt_signal(int number)
{
    if (number == SIGUSR1) fb_release = 1;
    if (number == SIGUSR2) fb_acquire = 1;
}

static int fb_restore_console(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct vt_mode mode = {.mode = VT_AUTO};
    int status = ioctl(fd, KDSETMODE, KD_TEXT);
    if (ioctl(fd, VT_SETMODE, &mode) < 0) status = -1;
    close(fd);
    return status;
}

static void fb_close(struct matrix_fb *f)
{
    if (f->graphics && ioctl(f->tty, KDSETMODE, KD_TEXT) < 0)
        perror("matrix-render: restore text mode");
    if (f->managed && ioctl(f->tty, VT_SETMODE, &f->old_vt) < 0)
        perror("matrix-render: restore VT mode");
    if (f->canvas.pixels) munmap(f->canvas.pixels, f->canvas.fix.smem_len);
    if (f->fd >= 0) close(f->fd);
    if (f->tty >= 0) close(f->tty);
    free(f->tiles);
    memset(f, 0, sizeof(*f));
    f->fd = f->tty = -1;
}

static int fb_make_tiles(struct matrix_fb *f, unsigned scale)
{
    if (scale < 1 || scale > 4) { errno = EINVAL; return -1; }
    f->width = 8 * scale;
    f->height = 16 * scale;
    f->cols = f->canvas.var.xres / f->width;
    f->rows = f->canvas.var.yres / f->height;
    if (!f->cols || !f->rows || f->cols > 65535 || f->rows > 65535) {
        errno = EINVAL;
        return -1;
    }
    f->tile_bytes = (size_t)f->width * f->height * f->canvas.bytes_per_pixel;
    f->tiles = malloc(256 * f->tile_bytes);
    if (!f->tiles) return -1;
    uint32_t black = rgb(&f->canvas, 0, 0, 0);
    uint32_t colors[] = {rgb(&f->canvas, 0, 190, 0), rgb(&f->canvas, 255, 255, 255)};
    for (unsigned bright = 0; bright < 2; ++bright) {
        for (unsigned ch = 0; ch < 128; ++ch) {
            unsigned char *tile = f->tiles + (bright * 128 + ch) * f->tile_bytes;
            for (unsigned y = 0; y < f->height; ++y) {
                for (unsigned x = 0; x < f->width; ++x) {
                    unsigned row = y / scale, col = x / scale;
                    int ink = glyphs[ch][row] & (1u << (7 - col));
                    uint32_t pixel = ink ? colors[bright] : black;
                    memcpy(tile, &pixel, f->canvas.bytes_per_pixel);
                    tile += f->canvas.bytes_per_pixel;
                }
            }
        }
    }
    return 0;
}

static int fb_start(struct matrix_fb *f, const char *device, const char *console, unsigned scale)
{
    f->fd = open(device, O_RDWR | O_CLOEXEC);
    if (f->fd < 0) goto fail;
    if (ioctl(f->fd, FBIOGET_FSCREENINFO, &f->canvas.fix) < 0 ||
        ioctl(f->fd, FBIOGET_VSCREENINFO, &f->canvas.var) < 0) goto fail;
    if (validate(&f->canvas) < 0) { errno = ENOTSUP; goto fail; }
    f->tty = open(console, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (f->tty < 0) goto fail;
    struct stat st;
    struct vt_stat vt;
    int mode;
    if (fstat(f->tty, &st) < 0 || ioctl(f->tty, VT_GETSTATE, &vt) < 0 ||
        ioctl(f->tty, KDGETMODE, &mode) < 0 || ioctl(f->tty, VT_GETMODE, &f->old_vt) < 0)
        goto fail;
    if (major(st.st_rdev) != 4 || minor(st.st_rdev) != vt.v_active ||
        mode != KD_TEXT || f->old_vt.mode != VT_AUTO) { errno = EBUSY; goto fail; }
    void *mapping = mmap(NULL, f->canvas.fix.smem_len, PROT_READ | PROT_WRITE,
                         MAP_SHARED, f->fd, 0);
    if (mapping == MAP_FAILED) goto fail;
    f->canvas.pixels = mapping;
    if (fb_make_tiles(f, scale) < 0) goto fail;
    struct sigaction action = {0};
    action.sa_handler = fb_vt_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) < 0 || sigaction(SIGUSR2, &action, NULL) < 0)
        goto fail;
    struct vt_mode owned = {.mode = VT_PROCESS, .relsig = SIGUSR1, .acqsig = SIGUSR2};
    if (ioctl(f->tty, VT_SETMODE, &owned) < 0) goto fail;
    f->managed = 1;
    if (ioctl(f->tty, KDSETMODE, KD_GRAPHICS) < 0) goto fail;
    f->graphics = f->active = 1;
    rectangle(&f->canvas, 0, 0, f->canvas.var.xres, f->canvas.var.yres,
              rgb(&f->canvas, 0, 0, 0));
    fprintf(stderr, "matrix-render: framebuffer %.16s, %ux%u pixels, %ux%u cells\n",
            f->canvas.fix.id, f->canvas.var.xres, f->canvas.var.yres, f->cols, f->rows);
    return 0;
fail:
    perror("matrix-render: framebuffer setup");
    fb_close(f);
    return -1;
}

/* Detect layout changes before writing with a stale stride or pixel format.
 * The service will restart and map the new layout if this happens. */
static int fb_layout_valid(struct matrix_fb *f)
{
    struct canvas now = {0};
    if (ioctl(f->fd, FBIOGET_FSCREENINFO, &now.fix) < 0 ||
        ioctl(f->fd, FBIOGET_VSCREENINFO, &now.var) < 0 || validate(&now) < 0)
        return 0;
    const struct fb_var_screeninfo *a = &now.var, *b = &f->canvas.var;
    return now.fix.smem_len == f->canvas.fix.smem_len &&
           now.fix.smem_start == f->canvas.fix.smem_start &&
           now.fix.line_length == f->canvas.fix.line_length &&
           a->xres == b->xres && a->yres == b->yres &&
           a->xoffset == b->xoffset && a->yoffset == b->yoffset &&
           a->bits_per_pixel == b->bits_per_pixel &&
           !memcmp(&a->red, &b->red, sizeof(a->red)) &&
           !memcmp(&a->green, &b->green, sizeof(a->green)) &&
           !memcmp(&a->blue, &b->blue, sizeof(a->blue)) &&
           !memcmp(&a->transp, &b->transp, sizeof(a->transp));
}

static int fb_events(struct matrix_fb *f, char *previous)
{
    if (fb_release) {
        fb_release = 0;
        if (ioctl(f->tty, VT_RELDISP, 1) < 0) return -1;
        f->active = 0;
    }
    if (fb_acquire) {
        fb_acquire = 0;
        if (ioctl(f->tty, VT_RELDISP, VT_ACKACQ) < 0 || !fb_layout_valid(f)) return -1;
        f->active = 1;
        memset(previous, 0, (size_t)f->cols * f->rows);
        rectangle(&f->canvas, 0, 0, f->canvas.var.xres, f->canvas.var.yres,
                  rgb(&f->canvas, 0, 0, 0));
    }
    return 0;
}

static size_t fb_render(struct matrix_fb *f, const char *rain, const char *heads,
                        const char *overlay, char *previous, char *previous_bright)
{
    if (!f->active) return 0;
    size_t changed = 0;
    size_t tile_row = (size_t)f->width * f->canvas.bytes_per_pixel;
    for (unsigned row = 0; row < f->rows; ++row) {
        for (unsigned col = 0; col < f->cols; ++col) {
            size_t cell = (size_t)row * f->cols + col;
            unsigned char ch = (unsigned char)(overlay[cell] ? overlay[cell] : rain[cell]);
            if (!ch) ch = ' ';
            if (ch >= 128) ch = '?';
            int bright = overlay[cell] || (row == 0 && heads[col]);
            if ((unsigned char)previous[cell] == ch && previous_bright[cell] == bright) continue;
            const unsigned char *tile = f->tiles + (bright * 128 + ch) * f->tile_bytes;
            unsigned char *destination = row_address(&f->canvas, row * f->height) + col * tile_row;
            for (unsigned y = 0; y < f->height; ++y) {
                memcpy(destination, tile, tile_row);
                destination += f->canvas.fix.line_length;
                tile += tile_row;
            }
            previous[cell] = (char)ch;
            previous_bright[cell] = (char)bright;
            ++changed;
        }
    }
    return changed;
}
#endif
