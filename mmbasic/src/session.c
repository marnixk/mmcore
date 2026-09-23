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

/* Release the interpreter-owned allocations (vars, graphics pages, sprites)
 * of one context. mmb_reset() operates on g_cur. */
static void console_free_context(mmb *m)
{
	mmb *save = g_cur;

	if (!m)
		return;
	g_cur = m;
	mmb_reset();
	g_cur = save;
}

void mmb_console_reset(void)
{
	const mmb_platform *plat = G.plat;
	int i;

	for (i = 1; i < MMB_MAX_CONSOLES; i++)
	{
		if (!g_mmb[i])
			continue;
		console_free_context(g_mmb[i]);
		if (plat && plat->free)
			plat->free(g_mmb[i]);
		g_mmb[i] = 0;
	}
	if (g_mmb[0])
	{
		console_free_context(g_mmb[0]);
		memset(g_mmb[0], 0, sizeof(mmb));
		g_mmb[0]->plat = plat;
		/* A fresh session starts at the ramdisk root, like boot. */
		g_mmb[0]->drive = 'A';
		strcpy(g_mmb[0]->cwd, "A:/");
	}
	memset(s_initialized, 0, sizeof(s_initialized));
	memset(s_shown, 0, sizeof(s_shown));
	s_pending = -1;
	g_console = 0;
	g_cur = g_mmb[0];
	s_initialized[0] = 1;
	s_shown[0] = 1; /* the warm reset paints the banner and prompt itself */
	mmb_front_reset();
}

/* A full-screen app owns the keyboard until it closes itself. The warm reset
 * reuses the interpreter in place, so without this the app stays active and the
 * REPL never gets the keyboard back (#606). Ask each app to leave through its
 * own key handler rather than reaching into its private state. */
static void warm_reset_close_apps(void)
{
	int guard;

	for (guard = 0; guard < 16; guard++)
	{
		if (mmb_in_ihelp())
			mmb_ihelp_key(27);          /* Esc */
		else if (mmb_in_package())
			mmb_package_key(27);
		else if (mmb_apptui_active())
			mmb_apptui_key(27);
		else if (mmb_settings_active())
			mmb_settings_key(27);
		/* These own the screen and leave on Alt+X. A modal does not eat the
		 * chord because the Alt prefix is decoded before the dialog. */
		else if (mmb_in_editor() || mmb_in_term() || mmb_in_wordpad() ||
			 mmb_in_sprite_edit() || mmb_in_connect())
		{
			if (mmb_in_editor())
			{
				mmb_editor_key(1);
				mmb_editor_key('x');
			}
			else if (mmb_in_term())
			{
				mmb_term_key(1);
				mmb_term_key('x');
			}
			else if (mmb_in_wordpad())
			{
				mmb_wordpad_key(1);
				mmb_wordpad_key('x');
			}
			else if (mmb_in_sprite_edit())
			{
				mmb_sprite_edit_key(1);
				mmb_sprite_edit_key('x');
			}
			else
			{
				mmb_connect_key(1);
				mmb_connect_key('x');
			}
		}
		else if (mmb_in_files())
			mmb_files_key(27);
		else if (mmb_in_paint())
			mmb_paint_key(27);
		else if (mmb_in_afk())
			mmb_afk_key(27);
		else if (mmb_in_juke())
			mmb_juke_key(27);
		else
			break;
	}
}

/* Ctrl+Alt+Del: re-initialise the interpreter in place. A hardware reset
 * restarts the whole SoC but can leave the USB controller unusable on a Pi,
 * so the prompt never returns (#577). A warm reset keeps the display and
 * keyboard attached and always lands at a ready prompt. */
void mmb_warm_reset(void)
{
	G.running = 0;
	mmb_play_stop();
	mmb_close_tcp_files();
	mmb_settings_save();

	/* Hand the keyboard back before the interpreter state is replaced. */
	warm_reset_close_apps();

	mmb_console_reset();
	mmb_gfx_init();
	mmb_settings_load();
	mmb_gfx_apply_default_mode();
	mmb_audio_apply_options();
	mmb_console_apply_colour();

	mmb_print_startup();
	mmb_boot_start();
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

	/* Retune the display to this console's MODE before repainting it: the
	 * hardware is still sized for the console we are leaving (#580). */
	mmb_gfx_reapply_mode();

	/* The saved screen does not carry the terminal pen: re-apply this
	 * console's COLOUR so the next characters use its foreground/background
	 * (#580). Do it before the restore so a terminal repaint cannot cover a
	 * TUI console's restored framebuffer. */
	mmb_console_apply_colour();

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
