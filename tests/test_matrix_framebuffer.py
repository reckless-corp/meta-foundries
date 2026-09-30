"""Test the framebuffer drawing code without opening any display devices."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "meta-project/recipes-core/matrix-console/files/matrix-render.c"


class MatrixFramebufferTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="matrix-fb-test-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = str(Path(cls.build.name) / "matrix-fb")
        flags = shlex.split(os.environ.get("MATRIX_TEST_CFLAGS", "-O2"))
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                        str(SOURCE), "-o", cls.binary], check=True)
        harness = Path(cls.build.name) / "test.c"
        harness.write_text("#define main renderer_main\n#include " + json.dumps(str(SOURCE)) + r'''
#undef main
#include <assert.h>
#include <stdarg.h>
extern int __real_close(int);
static struct canvas make_canvas(unsigned bpp) {
    struct canvas c = {0};
    c.fix.type = FB_TYPE_PACKED_PIXELS;
    c.fix.visual = FB_VISUAL_TRUECOLOR;
    c.var.xres = 64; c.var.yres = 40;
    c.var.xoffset = 3; c.var.yoffset = 2;
    c.var.xres_virtual = 70; c.var.yres_virtual = 45;
    c.var.bits_per_pixel = bpp;
    c.var.red = (struct fb_bitfield){bpp == 16 ? 11 : 16, bpp == 16 ? 5 : 8, 0};
    c.var.green = (struct fb_bitfield){bpp == 16 ? 5 : 8, bpp == 16 ? 6 : 8, 0};
    c.var.blue = (struct fb_bitfield){0, bpp == 16 ? 5 : 8, 0};
    if (bpp == 32) c.var.transp = (struct fb_bitfield){24, 8, 0};
    c.fix.line_length = 70 * (bpp / 8) + 13; /* Deliberately unaligned padding. */
    c.fix.smem_len = c.fix.line_length * 45;
    assert(validate(&c) == 0);
    return c;
}

static uint32_t read_pixel(struct canvas *c, unsigned x, unsigned y) {
    uint32_t value = 0;
    memcpy(&value, row_address(c, y) + x * c->bytes_per_pixel, c->bytes_per_pixel);
    return value;
}

/* Fake only the device-facing syscalls; exercise the actual main/cleanup path. */
static unsigned char device_memory[20000];
static int scenario, device_mode, opens, closes, vt_mode;
int __wrap_open(const char *path, int flags, ...) {
    (void)flags;
    ++opens;
    return !strcmp(path, "/dev/fb0") ? 50 : 51;
}
int __wrap_close(int fd) { (void)fd; ++closes; return 0; }
int __wrap_fstat(int fd, struct stat *st) {
    (void)fd;
    memset(st, 0, sizeof(*st)); st->st_rdev = makedev(4, 1); return 0;
}
int __wrap_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    va_list args; va_start(args, request);
    struct canvas c = make_canvas(32);
    int result = 0;
    if (request == KDSETMODE) {
        int mode = va_arg(args, int);
        if (scenario == 3 && mode == KD_GRAPHICS) { errno = EIO; result = -1; }
        else device_mode = mode;
    } else if (request == VT_RELDISP) {
        (void)va_arg(args, int);
    } else {
        void *value = va_arg(args, void *);
        if (request == FBIOGET_FSCREENINFO) *(struct fb_fix_screeninfo *)value = c.fix;
        else if (request == FBIOGET_VSCREENINFO) *(struct fb_var_screeninfo *)value = c.var;
        else if (request == KDGETMODE) *(int *)value = device_mode;
        else if (request == VT_GETSTATE) ((struct vt_stat *)value)->v_active = 1;
        else if (request == VT_GETMODE) ((struct vt_mode *)value)->mode = vt_mode;
        else if (request == VT_SETMODE) vt_mode = ((struct vt_mode *)value)->mode;
        else assert(0);
    }
    va_end(args); return result;
}
void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t offset) {
    (void)p; (void)n; (void)prot; (void)flags; (void)fd; (void)offset;
    if (scenario == 2) { errno = ENOMEM; return MAP_FAILED; }
    return device_memory;
}
int __wrap_munmap(void *p, size_t n) { (void)p; (void)n; return 0; }

int __wrap_nanosleep(const struct timespec *request, struct timespec *remaining) {
    (void)request; (void)remaining;
    assert(device_mode == KD_GRAPHICS && vt_mode == VT_PROCESS);
    /* One-letter artwork is centered at cell (3,0) on an 8x2 grid. */
    struct canvas c = make_canvas(32); c.pixels = device_memory;
    assert(read_pixel(&c, 25, 2) == rgb(&c, 255, 255, 255));
    stop(SIGTERM);
    return 0;
}

int main(void) {
    for (unsigned bpp = 16; bpp <= 32; bpp += 8) {
      for (unsigned scale = 1; scale <= 2; ++scale) {
        struct matrix_fb f = {.fd = -1, .tty = -1, .active = 1};
        f.canvas = make_canvas(bpp);
        struct canvas *c = &f.canvas;
        unsigned char *allocation = malloc(c->fix.smem_len + 32);
        assert(allocation);
        memset(allocation, 0xa5, c->fix.smem_len + 32);
        c->pixels = allocation + 16;
        assert(fb_make_tiles(&f, scale) == 0);
        assert(f.width == 8*scale && f.height == 16*scale);
        size_t n = (size_t)f.cols * f.rows;
        char *rain = calloc(n, 1), *heads = calloc(f.cols, 1), *overlay = calloc(n, 1);
        char *previous = calloc(n, 1), *bright = calloc(n, 1);
        rain[0] = 'F'; rain[n - 1] = 'A';
        assert(fb_render(&f, rain, heads, overlay, previous, bright) == n);
        assert(read_pixel(c, scale, 2*scale) == rgb(c, 0, 190, 0));
        assert(read_pixel(c, 0, 0) == rgb(c, 0, 0, 0));
        assert(fb_render(&f, rain, heads, overlay, previous, bright) == 0);
        heads[0] = 1;
        assert(fb_render(&f, rain, heads, overlay, previous, bright) == 1);
        assert(read_pixel(c, scale, 2*scale) == rgb(c, 255, 255, 255));
        overlay[n - 1] = 'F';
        assert(fb_render(&f, rain, heads, overlay, previous, bright) == 1);
        assert(read_pixel(c, (f.cols-1)*f.width + scale, (f.rows-1)*f.height + 2*scale) == rgb(c, 255,255,255));
        heads[0] = 0; rain[0] = ' ';
        assert(fb_render(&f, rain, heads, overlay, previous, bright) == 1);
        assert(read_pixel(c, scale, 2*scale) == rgb(c, 0, 0, 0));
        f.active = 0; rain[0] = 'F';
        assert(fb_render(&f, rain, heads, overlay, previous, bright) == 0);
        assert(read_pixel(c, scale, 2*scale) == rgb(c, 0, 0, 0));
        assert(memcmp(f.tiles + 'a'*f.tile_bytes, f.tiles + 'A'*f.tile_bytes, f.tile_bytes));
        for (size_t i = 0; i < c->fix.smem_len; ++i) {
            size_t y = i / c->fix.line_length, x = i % c->fix.line_length;
            if (y < 2 || y >= 2 + f.rows*f.height || x < 3*c->bytes_per_pixel ||
                x >= (3 + f.cols*f.width)*c->bytes_per_pixel)
                assert(c->pixels[i] == 0xa5);
        }
        for (unsigned i = 0; i < 16; ++i) {
            assert(allocation[i] == 0xa5);
            assert(allocation[c->fix.smem_len + 16 + i] == 0xa5);
        }
        free(allocation); free(f.tiles); free(rain); free(heads); free(overlay);
        free(previous); free(bright);
      }
    }
    for (scenario = 0; scenario < 4; ++scenario) {
        struct matrix_fb f = {.fd = -1, .tty = -1};
        device_mode = KD_TEXT; vt_mode = VT_AUTO; opens = closes = 0;
        int status = fb_start(&f, "/dev/fb0", "/dev/tty1", 2);
        assert(status == (scenario >= 2 ? -1 : 0));
        if (status == 0) {
            assert(device_mode == KD_GRAPHICS && vt_mode == VT_PROCESS);
            char previous[100]; memset(previous, 'A', sizeof(previous));
            fb_vt_signal(SIGUSR1);
            assert(fb_events(&f, previous) == 0 && !f.active);
            fb_vt_signal(SIGUSR2);
            assert(fb_events(&f, previous) == 0 && f.active);
            assert(previous[0] == 0);
            ++f.canvas.fix.line_length;
            assert(!fb_layout_valid(&f));
            --f.canvas.fix.line_length;
        }
        fb_close(&f);
        assert(device_mode == KD_TEXT && vt_mode == VT_AUTO);
        assert(opens == closes);
        fb_close(&f); /* Cleanup is idempotent. */
    }
    /* Exercise the real animation loop, artwork placement and signal cleanup. */
    char artwork[] = "/tmp/matrix-artwork-XXXXXX";
    int art_fd = mkstemp(artwork);
    assert(art_fd >= 0 && write(art_fd, "F\n", 2) == 2);
    __real_close(art_fd);
    scenario = 0; device_mode = KD_TEXT; vt_mode = VT_AUTO;
    opens = closes = 0; stopping = 0;
    char *args[] = {"matrix-render", artwork, NULL};
    assert(renderer_main(2, args) == 0);
    assert(device_mode == KD_TEXT && vt_mode == VT_AUTO && opens == closes);
    assert(unlink(artwork) == 0);
    puts("Framebuffer tiles, bounds, dirty cells, VT switching and cleanup passed");
    return 0;
}
''')
        cls.harness = str(Path(cls.build.name) / "test")
        wrappers = ["open", "close", "ioctl", "fstat", "mmap", "munmap", "nanosleep"]
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                        *[f"-Wl,--wrap={name}" for name in wrappers],
                        str(harness), "-o", cls.harness], check=True)

    def test_pixels_and_bounds(self):
        subprocess.run([self.harness], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
