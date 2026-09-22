#include "mmb_priv.h"
#include "frontend.h"
#include "session.h"

#include <string.h>

int g_console = 0;

static int s_initialized[MMB_MAX_CONSOLES];
static int s_shown[MMB_MAX_CONSOLES];
static int s_pending = -1;

int mmb_console_switch_pending(void)
{
	return s_pending >= 0;
}

void mmb_console_init(void)
{
	memset(s_initialized, 0, sizeof(s_initialized));
	memset(s_shown, 0, sizeof(s_shown));
	g_console = 0;
	g_cur = g_mmb[0];
	s_initialized[0] = 1;
	s_shown[0] = 1; /* the host prints the boot banner and prompt */
	mmb_front_select(0);
}

int mmb_console_count(void)
{
	return MMB_MAX_CONSOLES;
}

int mmb_console_active(void)
{
	return g_console;
}

/* Bring up a fresh interpreter context for a console that has not run yet.
 * Options mirror the console we switched from so PROMPT/MODE/etc. match. */
static void console_bring_up(int idx, const mmb *from)
{
	mmb *save = g_cur;

	if (!g_mmb[idx] && from->plat && from->plat->alloc)
		g_mmb[idx] = (mmb *)from->plat->alloc(sizeof(mmb));
	if (!g_mmb[idx])
		return;
	g_cur = g_mmb[idx];
	memset(g_cur, 0, sizeof(mmb));
	g_cur->plat = from->plat;
	mmb_option_reset();
	g_cur->opt = from->opt;
	mmb_gfx_init();
	mmb_gfx_apply_default_mode();
	g_cur->timer_base = 0;
	g_cur->rnd_seed = 0x12345678u;
	/* A fresh console starts at the ramdisk root (mmb_vfs_init owns the
	 * shared VFS node table and must not run again per console). */
	g_cur->drive = 'A';
	strcpy(g_cur->cwd, "A:/");
	mmb_console_apply_colour();
	s_initialized[idx] = 1;

	g_cur = save;
}

static int console_do_switch(int idx)
{
	const mmb_platform *plat;
	const mmb *from;

	plat = G.plat;
	from = g_cur;

	/* Snapshot the current screen before bringing the target up: a fresh
	 * console's gfx init retunes the HDMI framebuffer and would otherwise
	 * wipe the screen we are leaving. */
	if (plat && plat->console_save)
		plat->console_save(g_console, mmb_front_in_app());

	if (!s_initialized[idx])
		console_bring_up(idx, from);
	if (!g_mmb[idx])
		return 0;

	g_console = idx;
	g_cur = g_mmb[idx];
	mmb_front_select(idx);

	if (plat && plat->console_restore)
		plat->console_restore(idx, mmb_front_in_app());
	else
		mmb_console_write("\x1b[H\x1b[2J");

	if (!s_shown[idx])
	{
		s_shown[idx] = 1;
		mmb_print_startup();
		mmb_front_prompt();
	}

	/* A console whose program was suspended resumes where it left off. */
	if (G.run_suspended)
		mmb_resume_program();
	return 1;
}

/* Ask to switch consoles. While a program is running the switch is deferred
 * to the next line boundary (the run loop suspends and the host loop calls
 * mmb_console_poll); otherwise it happens now. */
int mmb_console_switch(int idx)
{
	if (idx < 0 || idx >= MMB_MAX_CONSOLES)
		return 0;
	if (idx == g_console)
	{
		s_pending = -1;
		return 0;
	}
	if (mmb_is_running())
	{
		s_pending = idx;
		return 0;
	}
	return console_do_switch(idx);
}

/* Host loop hook: perform a switch deferred by a running program. */
int mmb_console_poll(void)
{
	int idx = s_pending;

	if (idx < 0 || mmb_is_running())
		return 0;
	s_pending = -1;
	return console_do_switch(idx);
}
