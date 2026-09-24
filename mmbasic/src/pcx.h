/*
 * PCX codec (#631) - ZSoft PCX version 5.
 *
 * Decodes 8-bit single-plane and 24-bit three-plane RLE images, and encodes
 * 8-bit single-plane RLE images with a 256-colour palette trailer. Serves the
 * PAINT save/load path and the FILES browser's `.PCX` preview.
 *
 * Public prototypes live in mmb_priv.h; this header carries the decode limits.
 */
#ifndef MMB_PCX_H
#define MMB_PCX_H

#include <stdint.h>

/* Largest image edge and pixel count accepted. The pixel cap bounds the
 * working buffers on the bare-metal target; malformed headers that claim more
 * are rejected before any allocation. */
#define MMB_PCX_MAX_DIM    4096
#define MMB_PCX_MAX_PIXELS (4u * 1024u * 1024u)

/* Decode a PCX file (8-bit 1-plane or 24-bit 3-plane, RLE or raw) into 32-bit
 * 0xAARRGGBB pixels; callers free *out with G.plat->free. Returns 0 on
 * success, -1 on malformed/unsupported input. */
int mmb_pcx_decode_rgba(const unsigned char *file, unsigned n,
			uint32_t **out, int *w, int *h);

/* Decode an 8-bit indexed PCX to palette indices plus its 768-byte palette.
 * 24-bit input is rejected. */
int mmb_pcx_decode_indexed(const unsigned char *file, unsigned n,
			   unsigned char **idx, int *w, int *h,
			   unsigned char *pal, int *ncol);

/* Encode an 8-bit indexed image as PCX v5, 1 plane, RLE. `pal` is 768 bytes
 * (NULL uses the fixed VGA palette). On success *out is a G.plat->alloc'd
 * whole file; free with G.plat->free. */
int mmb_pcx_encode_indexed(const unsigned char *idx, int w, int h,
			   const unsigned char *pal,
			   unsigned char **out, unsigned *out_len);

#endif /* MMB_PCX_H */
