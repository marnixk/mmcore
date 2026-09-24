#include "mmb_priv.h"
#include "frontend.h"

#include <string.h>

#define FE_HIST_MAX 32
#define FE_LINE_MAX 256

static mmb_front_emit_fn s_emit;
static void *s_ctx;

/* Line editor state is per console so each virtual console keeps its own
 * input line, cursor, and history. */
typedef struct fe_state {
	char line[FE_LINE_MAX];
	unsigned len, pos;
	char hist[FE_HIST_MAX][FE_LINE_MAX];
	unsigned hist_n;
	int hist_idx;
	char draft[FE_LINE_MAX];
	int esc;    /* 0 idle, 1 ESC, 2 CSI, 3 SS3 */
	int csi_arg;
	int sealed;
} fe_state;

static fe_state s_fe[MMB_MAX_CONSOLES];
static int s_active;

#define s_line    (s_fe[s_active].line)
#define s_len     (s_fe[s_active].len)
#define s_pos     (s_fe[s_active].pos)
#define s_hist    (s_fe[s_active].hist)
#define s_hist_n  (s_fe[s_active].hist_n)
#define s_hist_idx (s_fe[s_active].hist_idx)
#define s_draft   (s_fe[s_active].draft)
#define s_esc     (s_fe[s_active].esc)
#define s_csi_arg (s_fe[s_active].csi_arg)
#define s_sealed  (s_fe[s_active].sealed)

static void fe_emit(const char *s, unsigned n)
{
	if (s && n && s_emit)
		s_emit(s_ctx, s, n);
}

static void fe_puts(const char *s)
{
	if (s)
		fe_emit(s, (unsigned)strlen(s));
}

static int line_is_numbered(const char *s)
{
	if (!s)
		return 0;
	while (*s == ' ' || *s == '\t')
		s++;
	return *s >= '0' && *s <= '9';
}

/* ---- line editor -------------------------------------------------- */

static void line_go_end(void)
{
	while (s_pos < s_len)
	{
		fe_emit(&s_line[s_pos], 1);
		s_pos++;
	}
}

static void line_clear_vis(void)
{
	line_go_end();
	while (s_len > 0)
	{
		s_len--;
		fe_puts("\b \b");
	}
	s_pos = 0;
	s_line[0] = '\0';
}

static void line_replace(const char *s)
{
	unsigned i;

	line_clear_vis();
	if (!s)
		s = "";
	for (i = 0; s[i] && s_len < sizeof s_line - 1; i++)
	{
		s_line[s_len++] = s[i];
		fe_emit(&s[i], 1);
	}
	s_line[s_len] = '\0';
	s_pos = s_len;
}

static void line_left(void)
{
	if (s_pos == 0)
		return;
	s_pos--;
	fe_puts("\b");
}

static void line_right(void)
{
	if (s_pos >= s_len)
		return;
	fe_emit(&s_line[s_pos], 1);
	s_pos++;
}

static void line_home(void)
{
	while (s_pos > 0)
	{
		s_pos--;
		fe_puts("\b");
	}
}

static void line_insert(char c)
{
	unsigned i, rest;

	if (s_len >= sizeof s_line - 1)
		return;
	if (s_pos > s_len)
		s_pos = s_len;
	rest = s_len - s_pos;
	if (rest)
		memmove(s_line + s_pos + 1, s_line + s_pos, rest);
	s_line[s_pos] = c;
	s_len++;
	s_line[s_len] = '\0';
	for (i = s_pos; i < s_len; i++)
		fe_emit(&s_line[i], 1);
	for (i = 0; i < rest; i++)
		fe_puts("\b");
	s_pos++;
}

static void line_backspace(void)
{
	unsigned i, rest;

	if (s_pos == 0)
		return;
	s_pos--;
	rest = s_len - s_pos - 1;
	if (rest)
		memmove(s_line + s_pos, s_line + s_pos + 1, rest);
	s_len--;
	s_line[s_len] = '\0';
	fe_puts("\b");
	for (i = 0; i < rest; i++)
		fe_emit(&s_line[s_pos + i], 1);
	fe_puts(" ");
	for (i = 0; i < rest + 1; i++)
		fe_puts("\b");
}

static void line_delete(void)
{
	unsigned i, rest;

	if (s_pos >= s_len)
		return;
	rest = s_len - s_pos - 1;
	if (rest)
		memmove(s_line + s_pos, s_line + s_pos + 1, rest);
	s_len--;
	s_line[s_len] = '\0';
	for (i = 0; i < rest; i++)
		fe_emit(&s_line[s_pos + i], 1);
	fe_puts(" ");
	for (i = 0; i < rest + 1; i++)
		fe_puts("\b");
}

static void hist_add(const char *s)
{
	if (!s || !s[0])
		return;
	if (s_hist_n > 0 && strcmp(s_hist[s_hist_n - 1], s) == 0)
		return;
	if (s_hist_n >= (unsigned)FE_HIST_MAX)
	{
		memmove(s_hist[0], s_hist[1],
			(FE_HIST_MAX - 1) * sizeof s_hist[0]);
		s_hist_n = (unsigned)FE_HIST_MAX - 1;
	}
	strncpy(s_hist[s_hist_n], s, sizeof s_hist[0] - 1);
	s_hist[s_hist_n][sizeof s_hist[0] - 1] = '\0';
	s_hist_n++;
	s_hist_idx = -1;
}

static void hist_up(void)
{
	if (s_hist_n == 0)
		return;
	if (s_hist_idx < 0)
	{
		unsigned n = s_len;

		if (n >= sizeof s_draft)
			n = sizeof s_draft - 1;
		memcpy(s_draft, s_line, n);
		s_draft[n] = '\0';
		s_hist_idx = (int)s_hist_n - 1;
	}
	else if (s_hist_idx > 0)
		s_hist_idx--;
	line_replace(s_hist[s_hist_idx]);
}

static void hist_down(void)
{
	if (s_hist_idx < 0)
		return;
	if (s_hist_idx >= (int)s_hist_n - 1)
	{
		s_hist_idx = -1;
		line_replace(s_draft);
		return;
	}
	s_hist_idx++;
	line_replace(s_hist[s_hist_idx]);
}

static void handle_csi(char final)
{
	if (final == 'A')
		hist_up();
	else if (final == 'B')
		hist_down();
	else if (final == 'C')
		line_right();
	else if (final == 'D')
		line_left();
	else if (final == 'H')
		line_home();
	else if (final == 'F')
		line_go_end();
	else if (final == '~')
	{
		if (s_csi_arg == 3)
			line_delete();
		else if (s_csi_arg == 1 || s_csi_arg == 7)
			line_home();
		else if (s_csi_arg == 4 || s_csi_arg == 8)
			line_go_end();
	}
}

/* ---- app routing / execution -------------------------------------- */

int mmb_front_line_empty(void)
{
	return s_len == 0;
}

int mmb_front_in_app(void)
{
	return mmb_in_editor() || mmb_in_files() || mmb_in_wordpad() ||
	       mmb_in_term() || mmb_in_connect() || mmb_in_ihelp() ||
	       mmb_in_afk() || mmb_in_juke() ||
	       mmb_in_paint() || mmb_in_package() ||
	       mmb_settings_active() || mmb_apptui_active();
}

static void submit(void)
{
	const char *result;
	int numbered;

	fe_puts("\r"); /* echo CR so the console wraps */
	s_line[s_len] = '\0';
	hist_add(s_line);
	s_pos = 0;
	s_hist_idx = -1;

	result = mmb_exec_line(s_line);
	numbered = line_is_numbered(s_line);

	if (mmb_quit_requested())
	{
		/* QUIT: the host loop is about to end; no prompt. */
		s_len = 0;
		s_pos = 0;
		return;
	}

	if (mmb_program_suspended())
	{
		/* A virtual-console switch stopped RUN at a line boundary; the host
		 * loop performs the switch, so do not paint a prompt here. */
		s_len = 0;
		s_pos = 0;
		return;
	}

	if (mmb_in_editor())
	{
		if (result && result[0])
			fe_puts(result);
	}
	else if (mmb_take_home_prompt())
	{
		fe_puts(result);
		mmb_front_prompt();
	}
	else if (mmb_in_files() || mmb_in_ihelp() || mmb_in_package() ||
		 mmb_settings_active())
	{
		/* TUI already streamed to the screen. */
	}
	else if (mmb_in_term() || mmb_in_wordpad() || mmb_in_connect() ||
		 mmb_in_paint())
	{
		if (result && result[0])
			fe_puts(result);
	}
	else if (mmb_in_afk())
	{
		/* AFK owns the screen; no prompt. */
	}
	else if (!result || !result[0])
	{
		if (!numbered)
			fe_puts("\r\n");
		fe_puts("\r\n");
		mmb_front_prompt();
	}
	else
	{
		fe_puts("\r\n");
		fe_puts(result);
		if (!numbered)
			fe_puts("\r\n");
		fe_puts("\r\n"); /* the old emit_nl_prompt newline */
		mmb_front_prompt();
	}
	s_len = 0;
	s_pos = 0;
}

/* Global screenshot hotkey: F12 (ESC [ 2 4 ~). Consumed before any app or the
 * REPL sees it, so it works everywhere. The escape is buffered only while the
 * bytes still match the sequence; a mismatch replays the buffered prefix so
 * ordinary Alt/CSI input is byte-for-byte unchanged. A lone Esc is flushed by
 * mmb_front_poll() so app Esc timeouts still fire. */
#define FRONT_SHOT_SEQ "\x1b[24~"
#define FRONT_ESC_TIMEOUT_MS 40

static int s_shot_state;
static unsigned s_shot_at;

static void front_feed_dispatch(char c);

/* Flush a buffered but incomplete Esc sequence (host loop, from mmb_poll). */
void mmb_front_poll(void)
{
	char seq[sizeof FRONT_SHOT_SEQ];
	int i, n;

	if (s_shot_state <= 0)
		return;
	if (mmb_now_ms() - s_shot_at < FRONT_ESC_TIMEOUT_MS)
		return;
	memcpy(seq, FRONT_SHOT_SEQ, sizeof(FRONT_SHOT_SEQ));
	n = s_shot_state;
	s_shot_state = 0;
	for (i = 0; i < n; i++)
		front_feed_dispatch(seq[i]);
}

void mmb_front_feed_byte(char c)
{
	const char *seq = FRONT_SHOT_SEQ;

	if (s_shot_state > 0)
	{
		if (c == seq[s_shot_state])
		{
			s_shot_state++;
			if (!seq[s_shot_state])
			{
				char msg[24];
				s_shot_state = 0;
				if (mmb_screenshot_hotkey(msg, (int)sizeof(msg)) != 0)
					fe_puts(msg);
				return;
			}
			return;
		}
		{
			int i, n = s_shot_state;
			s_shot_state = 0;
			for (i = 0; i < n; i++)
				front_feed_dispatch(seq[i]);
		}
		front_feed_dispatch(c);
		return;
	}
	if (c == 0x1b)
	{
		s_shot_state = 1;
		s_shot_at = mmb_now_ms();
		return;
	}
	front_feed_dispatch(c);
}

static void front_feed_dispatch(char c)
{
	/* Full-screen apps own the keyboard. */
	if (mmb_in_ihelp())
	{
		fe_puts(mmb_ihelp_key(c));
		return;
	}
	if (mmb_in_package())
	{
		fe_puts(mmb_package_key(c));
		return;
	}
	if (mmb_apptui_active())
	{
		fe_puts(mmb_apptui_key(c));
		if (!mmb_apptui_active())
			mmb_front_prompt();
		return;
	}
	if (mmb_settings_active())
	{
		fe_puts(mmb_settings_key(c));
		if (!mmb_settings_active())
			mmb_front_prompt();
		return;
	}
	if (mmb_in_editor())
	{
		fe_puts(mmb_editor_key(c));
		if (!mmb_in_editor())
		{
			if (mmb_in_files())
			{
				fe_puts(mmb_files_on_editor_exit());
				if (!mmb_in_files())
					mmb_front_prompt();
			}
			else
				mmb_front_prompt();
		}
		return;
	}
	if (mmb_in_files())
	{
		fe_puts(mmb_files_key(c));
		if (!mmb_in_files() && !mmb_in_editor())
		{
			if (!mmb_files_take_prompt())
				mmb_front_prompt();
		}
		return;
	}
	if (mmb_in_term())
	{
		const char *out = mmb_term_key(c);

		if (out && out[0])
			fe_puts(out);
		if (!mmb_in_term())
			mmb_front_prompt();
		return;
	}
	if (mmb_in_wordpad())
	{
		const char *out = mmb_wordpad_key(c);

		if (out && out[0])
			fe_puts(out);
		if (!mmb_in_wordpad())
			mmb_front_prompt();
		return;
	}
	if (mmb_in_paint())
	{
		/* PAINT paints its own prompt on exit. */
		fe_puts(mmb_paint_key(c));
		return;
	}
	if (mmb_in_afk())
	{
		mmb_afk_key(c);
		if (!mmb_in_afk())
			mmb_front_prompt();
		return;
	}
	if (mmb_in_juke())
	{
		fe_puts(mmb_juke_key(c));
		if (!mmb_in_juke())
			mmb_front_prompt();
		return;
	}
	if (mmb_in_connect())
	{
		const char *out = mmb_connect_key(c);

		if (out && out[0])
			fe_puts(out);
		if (!mmb_in_connect())
			mmb_front_prompt();
		return;
	}

	/* Ctrl+Space: quick .APP picker at the REPL prompt (#520). */
	if (c == 0)
	{
		mmb_apptui_open(0);
		return;
	}

	/* ESC / CSI decoder. */
	if (s_esc == 1)
	{
		if (c == '[')
		{
			s_esc = 2;
			s_csi_arg = 0;
			return;
		}
		if (c == 'O')
		{
			s_esc = 3;
			return;
		}
		s_esc = 0;
		if (c == 0x1b)
			s_esc = 1;
		/* ESC+letter is Alt; ignore at the prompt. */
		return;
	}
	if (s_esc == 2)
	{
		if (c >= '0' && c <= '9')
		{
			s_csi_arg = s_csi_arg * 10 + (c - '0');
			return;
		}
		if (c == ';')
		{
			s_csi_arg = 0;
			return;
		}
		s_esc = 0;
		handle_csi(c);
		return;
	}
	if (s_esc == 3)
	{
		s_esc = 0;
		handle_csi(c);
		return;
	}
	if (c == 0x1b)
	{
		s_esc = 1;
		return;
	}

	if (c == '\r' || c == '\n')
		submit();
	else if (c == 8 || c == 127)
		line_backspace();
	else if (c == 3)
	{
		s_len = 0;
		s_pos = 0;
		s_hist_idx = -1;
		fe_puts("\r\n");
		mmb_front_prompt();
	}
	else if (c == '\t' || (unsigned char)c < 32)
	{
		/* Controls other than the ones above never enter the line. */
	}
	else
		line_insert(c);
}

void mmb_front_feed(const char *s, unsigned n)
{
	unsigned i;

	if (!s)
		return;
	for (i = 0; i < n; i++)
		mmb_front_feed_byte(s[i]);
}

void mmb_front_init(mmb_front_emit_fn emit, void *ctx)
{
	s_emit = emit;
	s_ctx = ctx;
	memset(s_fe, 0, sizeof(s_fe));
	s_active = 0;
	s_fe[0].hist_idx = -1;
	s_fe[0].line[0] = '\0';
}

/* Drop every console's line editor and history state. Used by a warm reset so
 * no partially typed line or app session survives. */
void mmb_front_reset(void)
{
	int i;

	for (i = 0; i < MMB_MAX_CONSOLES; i++)
	{
		memset(&s_fe[i], 0, sizeof(s_fe[i]));
		s_fe[i].hist_idx = -1;
	}
	s_active = 0;
}

/* Select which console's line editor receives keystrokes. */
void mmb_front_select(int idx)
{
	if (idx < 0 || idx >= MMB_MAX_CONSOLES)
		return;
	s_active = idx;
	if (s_fe[idx].hist_idx < -1 || s_fe[idx].hist_idx >= (int)s_fe[idx].hist_n)
		s_fe[idx].hist_idx = -1;
}

void mmb_front_prompt(void)
{
	/* A full-screen app owns the display: never paint the REPL prompt over
	 * it (stray emissions used to leave the prompt at the top-left of HELP
	 * and WORDPAD). A sealed CLI session never returns to the REPL at all. */
	if (s_sealed || mmb_front_in_app())
		return;
	mmb_hw_cursor(1);
	fe_puts(mmb_prompt());
}

void mmb_front_set_sealed(int on)
{
	s_sealed = on ? 1 : 0;
}

/* Portable pointer accessor: the platform backend owns the device. A backend
 * without a mouse (or a bare Pi with none attached) reports absence. */
int mmb_mouse_read(mmb_mouse_state *out)
{
	if (out)
	{
		out->present = 0;
		out->x = out->y = 0;
		out->buttons = 0;
		out->wheel = 0;
	}
	if (!out || !G.plat || !G.plat->mouse_state)
		return 0;
	return G.plat->mouse_state(out);
}
