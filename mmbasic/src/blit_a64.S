/*
 * AArch64 equivalents of x86 REP MOVSW / REP STOSD for 16-bit pixels.
 * Each LDP/STP moves eight COLOR16 pixels (16 bytes).
 */
	.text
	.align	4
	.global	mmb_blit_copy_u16
	.type	mmb_blit_copy_u16, %function
	.global	mmb_blit_copy_rect16
	.type	mmb_blit_copy_rect16, %function

/* x0 dst, x1 src, x2 n (count of uint16) */
mmb_blit_copy_u16:
	cbz	x2, 9f
	orr	x9, x0, x1
	tst	x9, #7
	b.ne	2f
	cmp	x2, #8
	b.lo	2f
1:
	ldp	x10, x11, [x1], #16
	sub	x2, x2, #8
	stp	x10, x11, [x0], #16
	cmp	x2, #8
	b.hs	1b
	cbz	x2, 9f
2:
	ldrh	w9, [x1], #2
	subs	x2, x2, #1
	strh	w9, [x0], #2
	b.ne	2b
9:
	ret

/*
 * x0 dst, x1 dst_stride_px, x2 src, x3 src_stride_px, w4 w, w5 h
 * Forward copy; dest and source rectangles must not overlap.
 */
mmb_blit_copy_rect16:
	cbz	w4, 9f
	cbz	w5, 9f
	sxtw	x4, w4
	sxtw	x5, w5
	sxtw	x1, w1
	sxtw	x3, w3
	lsl	x1, x1, #1
	lsl	x3, x3, #1
0:
	mov	x6, x0
	mov	x7, x2
	mov	x8, x4
	orr	x9, x6, x7
	tst	x9, #7
	b.ne	2f
	cmp	x8, #8
	b.lo	2f
1:
	ldp	x10, x11, [x7], #16
	sub	x8, x8, #8
	stp	x10, x11, [x6], #16
	cmp	x8, #8
	b.hs	1b
	cbz	x8, 3f
2:
	ldrh	w9, [x7], #2
	subs	x8, x8, #1
	strh	w9, [x6], #2
	b.ne	2b
3:
	add	x0, x0, x1
	add	x2, x2, x3
	subs	x5, x5, #1
	b.ne	0b
9:
	ret
