/* JPEG XL / PPM / PBM decoding for TERM image verbs.
 *
 * The bare-metal decoder is J40 (third_party/j40.h), a single-file public
 * domain C99 JPEG XL decoder.  Everything here is silent on failure: the
 * caller drops the frame rather than printing anything.
 */
#ifndef MMB_JXL_DEC_H
#define MMB_JXL_DEC_H

#include <stddef.h>

typedef struct {
	unsigned char *rgb; /* tightly packed RGB888, rows top-down */
	int w, h;
} mmb_img_rgb;

/* Decode a JPEG XL codestream or container.  Rejects images larger than
 * max_pixels before allocating the output.  Returns 0 on success. */
int mmb_jxl_decode(const unsigned char *src, size_t len,
		   unsigned max_pixels, mmb_img_rgb *out);

/* Decode PPM (P3/P6), PGM (P2/P5) or PBM (P1/P4) to RGB888. */
int mmb_ppm_decode(const unsigned char *src, size_t len,
		   unsigned max_pixels, mmb_img_rgb *out);

/* Probe the JPEG XL size header without decoding pixels.  Returns 0 when the
 * codestream header is intact. */
int mmb_jxl_probe(const unsigned char *src, size_t len, int *w, int *h);

void mmb_img_free(mmb_img_rgb *img);

#endif
