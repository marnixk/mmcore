/*
 * mmcore-splash - paint the mmcore logo on the Linux framebuffer.
 *
 * The live and installed systems boot with `console=tty0`, so the kernel's
 * framebuffer console binds to tty1 and erases whatever GRUB drew. This tiny
 * helper repaints the same artwork (a P6 PPM baked into the rootfs by
 * png_to_ppm.py) until mmcore modesets its own KMS surface, closing the black
 * gap between the bootloader and the application (#902).
 *
 * Usage:
 *   mmcore-splash [-i SECONDS] [-d DEV] [IMAGE]
 *
 *   -i SECONDS   keep redrawing every SECONDS until killed (default: draw once)
 *   -d DEV       framebuffer device to use (default: first working /dev/fb?N)
 *   IMAGE        P6 PPM to draw (default /usr/share/mmcore/splash.ppm)
 *
 * The helper writes the whole visible frame: black everywhere, with the image
 * centred at its native size. It understands the fbdev bit-field layout, so it
 * works on 16/24/32 bpp framebuffers.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEFAULT_IMAGE "/usr/share/mmcore/splash.ppm"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* Read one decimal token from a PPM header, skipping whitespace/comments. */
static int ppm_int(FILE *f, long *out)
{
	int c;

	do {
		c = fgetc(f);
		if (c == '#') {
			while (c != EOF && c != '\n')
				c = fgetc(f);
		}
	} while (c == '\n' || c == '\r' || c == ' ' || c == '\t');

	if (c == EOF)
		return -1;

	{
		long v = 0;
		int got = 0;

		while (c >= '0' && c <= '9') {
			v = v * 10 + (c - '0');
			got = 1;
			c = fgetc(f);
		}
		if (!got)
			return -1;
		if (c != EOF)
			ungetc(c, f);
		*out = v;
	}
	return 0;
}

/* Load a binary P6 PPM. Returns malloc'd RGB bytes, or NULL on error. */
static unsigned char *read_ppm(const char *path, long *out_w, long *out_h)
{
	FILE *f = fopen(path, "rb");
	unsigned char *pixels;
	long w, h, maxval;
	size_t n;

	if (!f)
		return NULL;

	if (fgetc(f) != 'P' || fgetc(f) != '6')
		goto fail;
	if (ppm_int(f, &w) != 0 || ppm_int(f, &h) != 0 || ppm_int(f, &maxval) != 0)
		goto fail;
	if (w <= 0 || h <= 0 || maxval != 255 || w > 16384 || h > 16384)
		goto fail;

	/* Exactly one whitespace byte separates the header from the raster. */
	if (fgetc(f) == EOF)
		goto fail;

	n = (size_t)w * (size_t)h * 3;
	pixels = malloc(n);
	if (!pixels)
		goto fail;
	if (fread(pixels, 1, n, f) != n) {
		free(pixels);
		goto fail;
	}
	fclose(f);
	*out_w = w;
	*out_h = h;
	return pixels;

fail:
	fclose(f);
	return NULL;
}

/* Scale an 8-bit channel to a framebuffer bit-field and place it. */
static uint32_t place(unsigned v, const struct fb_bitfield *b)
{
	uint32_t max, scaled;

	if (!b->length)
		return 0;
	max = (1u << b->length) - 1u;
	scaled = ((uint32_t)v * max + 127u) / 255u;
	return scaled << b->offset;
}

/* Store one pixel in the framebuffer's byte order (x86_64 is little-endian). */
static void store_pixel(unsigned char *dst, uint32_t value, int bytes)
{
	int i;

	for (i = 0; i < bytes; i++)
		dst[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
}

static int open_fb(const char *dev)
{
	if (dev && *dev)
		return open(dev, O_RDWR);
	{
		char path[32];
		int i;

		for (i = 0; i < 4; i++) {
			int fd;

			snprintf(path, sizeof(path), "/dev/fb%d", i);
			fd = open(path, O_RDWR);
			if (fd >= 0)
				return fd;
		}
	}
	return -1;
}

static int draw_once(const unsigned char *rgb, long w, long h, const char *dev)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	unsigned char *frame;
	long x0, y0, j, i, cw, ch, sx, sy;
	int fd, bytes, row;

	fd = open_fb(dev);
	if (fd < 0)
		return -1;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		close(fd);
		return -1;
	}
	if (var.bits_per_pixel < 8 || var.bits_per_pixel > 32 ||
	    var.bits_per_pixel % 8 != 0 || var.xres <= 0 || var.yres <= 0) {
		close(fd);
		return -1;
	}
	bytes = var.bits_per_pixel / 8;
	row = fix.line_length ? (int)fix.line_length
			      : (int)(var.xres * (unsigned)bytes);

	frame = calloc((size_t)row * var.yres, 1);
	if (!frame) {
		close(fd);
		return -1;
	}

	/* Clip an oversized image and centre what is left. */
	cw = w < var.xres ? w : var.xres;
	ch = h < var.yres ? h : var.yres;
	sx = (w - cw) / 2;
	sy = (h - ch) / 2;
	x0 = (var.xres - cw) / 2;
	y0 = (var.yres - ch) / 2;

	for (j = 0; j < ch; j++) {
		unsigned char *drow = frame + (y0 + j) * row + x0 * bytes;
		const unsigned char *srow = rgb + ((sy + j) * w + sx) * 3;

		for (i = 0; i < cw; i++) {
			unsigned r = srow[i * 3 + 0];
			unsigned g = srow[i * 3 + 1];
			unsigned b = srow[i * 3 + 2];
			uint32_t p = place(r, &var.red) | place(g, &var.green) |
				     place(b, &var.blue);

			store_pixel(drow + i * bytes, p, bytes);
		}
	}

	if (lseek(fd, 0, SEEK_SET) < 0 ||
	    write(fd, frame, (size_t)row * var.yres) < 0) {
		free(frame);
		close(fd);
		return -1;
	}
	free(frame);
	close(fd);
	return 0;
}

/*
 * #921: paint only while tty1 is the active virtual terminal. If the stop path
 * ever misses the process, this keeps the logo from stomping another console
 * (e.g. the tty2 shell) forever. Defaults to painting when the state cannot be
 * read, matching the pre-#921 behaviour on systems without /dev/tty0.
 */
static int tty1_is_active(void)
{
	struct vt_stat st = { 0 };
	int fd = open("/dev/tty0", O_RDONLY | O_NONBLOCK);
	int active = 1;

	if (fd < 0)
		return 1;
	if (ioctl(fd, VT_GETSTATE, &st) == 0)
		active = (st.v_active == 1);
	close(fd);
	return active;
}

int main(int argc, char **argv)
{
	const char *image = DEFAULT_IMAGE;
	const char *dev = NULL;
	int interval = 0;
	unsigned char *rgb;
	long w = 0, h = 0;
	int opt;

	while ((opt = getopt(argc, argv, "i:d:h")) != -1) {
		switch (opt) {
		case 'i':
			interval = atoi(optarg);
			if (interval < 0)
				interval = 0;
			break;
		case 'd':
			dev = optarg;
			break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: mmcore-splash [-i SECONDS] [-d DEV] [IMAGE]\n");
			return opt == 'h' ? 0 : 2;
		}
	}
	if (optind < argc)
		image = argv[optind];

	rgb = read_ppm(image, &w, &h);
	if (!rgb) {
		fprintf(stderr, "mmcore-splash: cannot read %s: %s\n", image,
			strerror(errno));
		return 1;
	}

	if (interval == 0) {
		int rc = draw_once(rgb, w, h, dev);

		free(rgb);
		return rc == 0 ? 0 : 1;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	while (!g_stop) {
		int i;

		/* Ignore a not-yet-registered /dev/fb0: retry on the next tick. */
		if (tty1_is_active())
			(void)draw_once(rgb, w, h, dev);
		for (i = 0; i < interval * 10 && !g_stop; i++)
			usleep(100 * 1000);
	}
	free(rgb);
	return 0;
}
