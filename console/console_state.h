/*
 * Virtual-console framebuffer snapshot bookkeeping (mmbasic-console-state).
 *
 * A console slot keeps an allocated snapshot buffer whose capacity grows to
 * the largest mode it has ever captured. The number of bytes actually
 * captured for the current mode can therefore be smaller than that capacity.
 * Restore must copy only the captured bytes and only when they describe the
 * live geometry; copying the stale capacity overruns a framebuffer that
 * shrank with the mode (#786).
 */
#ifndef MMB_CONSOLE_STATE_H
#define MMB_CONSOLE_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bytes to copy from a saved framebuffer snapshot into the live framebuffer,
 * or 0 when the snapshot must not be used.
 *
 * saved_pitch/saved_rows describe the geometry the snapshot was captured at,
 * saved_len is how many bytes were actually captured, and live_pitch/live_rows
 * describe the framebuffer right now. A match requires both the geometry and
 * the captured length to line up, so a stale larger capacity is rejected. The
 * result never exceeds the live framebuffer's visible size.
 */
static unsigned mmb_console_fb_restore_len(unsigned saved_pitch,
					   unsigned saved_rows,
					   unsigned saved_len,
					   unsigned live_pitch,
					   unsigned live_rows)
{
	unsigned need;

	if (saved_pitch != live_pitch || saved_rows != live_rows)
		return 0;
	need = saved_pitch * saved_rows;
	if (need == 0 || saved_len != need)
		return 0;
	return need;
}

#ifdef __cplusplus
}
#endif

#endif /* MMB_CONSOLE_STATE_H */
