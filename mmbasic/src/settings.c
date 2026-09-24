#include "mmb_priv.h"

#define SETTINGS_MAX 4096

static char settings_path[32];

static const char *keyboard_name(int lang)
{
	switch (lang)
	{
	case 1: return "UK";
	case 2: return "DE";
	case 3: return "FR";
	case 4: return "ES";
	default: return "US";
	}
}

static int keyboard_id(const char *s)
{
	if (mmb_keyword_eq(s, "UK"))
		return 1;
	if (mmb_keyword_eq(s, "DE"))
		return 2;
	if (mmb_keyword_eq(s, "FR"))
		return 3;
	if (mmb_keyword_eq(s, "ES"))
		return 4;
	return 0;
}

static void choose_path(void)
{
	if (mmb_fat_ready('C'))
		strncpy(settings_path, "C:/.mmbasic.ini", sizeof(settings_path) - 1);
	else
		strncpy(settings_path, "A:/.mmbasic.ini", sizeof(settings_path) - 1);
	settings_path[sizeof(settings_path) - 1] = 0;
}

const char *mmb_settings_path(void)
{
	if (!settings_path[0])
		choose_path();
	return settings_path;
}

static void append(char *buf, int sz, const char *s)
{
	int n = (int)strlen(buf);
	int i;
	for (i = 0; s[i] && n + 1 < sz; i++)
		buf[n++] = s[i];
	buf[n] = 0;
}

static void append_int(char *buf, int sz, int64_t n)
{
	char tmp[24];
	char *p = tmp + sizeof(tmp) - 1;
	int neg = 0;
	uint64_t v;
	*p = 0;
	if (n < 0)
	{
		neg = 1;
		v = (uint64_t)(-n);
	}
	else
		v = (uint64_t)n;
	if (v == 0)
		*--p = '0';
	while (v)
	{
		*--p = (char)('0' + (v % 10));
		v /= 10;
	}
	if (neg)
		*--p = '-';
	append(buf, sz, p);
}

static void kv_int(char *buf, int sz, const char *k, int64_t v)
{
	append(buf, sz, k);
	append(buf, sz, "=");
	append_int(buf, sz, v);
	append(buf, sz, "\n");
}

static void kv_str(char *buf, int sz, const char *k, const char *v)
{
	append(buf, sz, k);
	append(buf, sz, "=");
	if (v)
		append(buf, sz, v);
	append(buf, sz, "\n");
}

static int parse_int(const char *s)
{
	int n = 0, sign = 1;
	if (*s == '-')
	{
		sign = -1;
		s++;
	}
	while (*s >= '0' && *s <= '9')
		n = n * 10 + (*s++ - '0');
	return sign * n;
}

static double parse_vcc(const char *s)
{
	int whole = 0, frac = 0, scale = 1, after = 0;
	if (*s == '-')
		s++;
	while (*s)
	{
		if (*s == '.')
		{
			after = 1;
			s++;
			continue;
		}
		if (*s < '0' || *s > '9')
			break;
		if (after)
		{
			if (scale < 1000)
			{
				frac = frac * 10 + (*s - '0');
				scale *= 10;
			}
		}
		else
			whole = whole * 10 + (*s - '0');
		s++;
	}
	return (double)whole + (scale > 1 ? (double)frac / (double)scale : 0);
}

static void trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
	{
		memmove(s, s + 1, strlen(s));
	}
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = 0;
}

/* Which theme keys the file carried, so old configs can be migrated onto the
 * split system/editor theme options (see mmb_settings_load). */
static int saw_theme_key, saw_edit_theme_key;

static void apply_core(const char *k, const char *v)
{
	int i;
	if (mmb_keyword_eq(k, "keyboard"))
		G.opt.keyboard_lang = keyboard_id(v);
	else if (mmb_keyword_eq(k, "keyboard_noled"))
		G.opt.keyboard_noled = parse_int(v);
	else if (mmb_keyword_eq(k, "repeat_first"))
		G.opt.repeat_first = parse_int(v);
	else if (mmb_keyword_eq(k, "repeat_next"))
		G.opt.repeat_next = parse_int(v);
	else if (mmb_keyword_eq(k, "tab"))
		G.opt.tab = parse_int(v);
	else if (mmb_keyword_eq(k, "break"))
		G.opt.break_key = parse_int(v);
	else if (mmb_keyword_eq(k, "autorun"))
		G.opt.autorun = parse_int(v);
	else if (mmb_keyword_eq(k, "colourcode"))
		G.opt.colourcode = parse_int(v);
	else if (mmb_keyword_eq(k, "colourcode_reverse"))
		G.opt.colourcode_reverse = parse_int(v);
	else if (mmb_keyword_eq(k, "console"))
		G.opt.console = parse_int(v);
	else if (mmb_keyword_eq(k, "console_port"))
		G.opt.console_port = parse_int(v);
	else if (mmb_keyword_eq(k, "crlf"))
		G.opt.crlf = parse_int(v);
	else if (mmb_keyword_eq(k, "default_mode"))
		G.opt.default_mode = parse_int(v);
	else if (mmb_keyword_eq(k, "baudrate"))
		G.opt.baudrate = parse_int(v);
	else if (mmb_keyword_eq(k, "case"))
		G.opt.case_mode = parse_int(v);
	else if (mmb_keyword_eq(k, "legacy"))
		G.opt.legacy = parse_int(v);
	else if (mmb_keyword_eq(k, "milliseconds"))
		G.opt.milliseconds = parse_int(v);
	else if (mmb_keyword_eq(k, "mouse"))
		G.opt.mouse = parse_int(v);
	else if (mmb_keyword_eq(k, "mouse_sens"))
		G.opt.mouse_sens = parse_int(v);
	else if (mmb_keyword_eq(k, "pin"))
		G.opt.pin = parse_int(v);
	else if (mmb_keyword_eq(k, "profiling"))
		G.opt.profiling = parse_int(v);
	else if (mmb_keyword_eq(k, "prompt"))
		G.opt.prompt = parse_int(v) ? 1 : 0;
	else if (mmb_keyword_eq(k, "status"))
		G.opt.status = parse_int(v);
	else if (mmb_keyword_eq(k, "vcc"))
		G.opt.vcc = parse_vcc(v);
	else if (mmb_keyword_eq(k, "sleep"))
		G.opt.sleep_min = parse_int(v);
	else if (mmb_keyword_eq(k, "sd_fast"))
		G.opt.sd_fast = parse_int(v);
	else if (mmb_keyword_eq(k, "serial_pullup"))
		G.opt.serial_pullup = parse_int(v);
	else if (mmb_keyword_eq(k, "rtc_cal"))
		G.opt.rtc_cal = parse_int(v);
	else if (mmb_keyword_eq(k, "ds3231"))
		G.opt.ds3231 = parse_int(v);
	else if (mmb_keyword_eq(k, "baseline"))
		G.opt.baseline = parse_int(v);
	else if (mmb_keyword_eq(k, "edit_font"))
		G.opt.edit_font = parse_int(v);
	else if (mmb_keyword_eq(k, "theme"))
	{
		saw_theme_key = 1;
		G.opt.theme = parse_int(v);
	}
	else if (mmb_keyword_eq(k, "edit_theme"))
	{
		saw_edit_theme_key = 1;
		G.opt.edit_theme = parse_int(v);
	}
	else if (mmb_keyword_eq(k, "edit_jump_break"))
		G.opt.edit_jump_break = parse_int(v) ? 1 : 0;
	else if (mmb_keyword_eq(k, "y_axis_up"))
		G.opt.y_axis_up = parse_int(v);
	else if (mmb_keyword_eq(k, "angle_degrees"))
		G.opt.angle_degrees = parse_int(v);
	else if (mmb_keyword_eq(k, "error_continue"))
		G.opt.error_continue = parse_int(v);
	else if (mmb_keyword_eq(k, "audio_on"))
		G.opt.audio_on = parse_int(v);
	else if (mmb_keyword_eq(k, "audio_target"))
		G.opt.audio_target = parse_int(v) ? 1 : 0;
	else if (mmb_keyword_eq(k, "term_log"))
		G.opt.term_log = parse_int(v) ? 1 : 0;
	else if (mmb_keyword_eq(k, "term_scrollback"))
	{
		G.opt.term_scrollback = parse_int(v);
		if (G.opt.term_scrollback < 0)
			G.opt.term_scrollback = 0;
		if (G.opt.term_scrollback > 256)
			G.opt.term_scrollback = 256;
	}
	else if (mmb_keyword_eq(k, "term_autolog"))
		G.opt.term_autolog = parse_int(v) ? 1 : 0;
	else if (mmb_keyword_eq(k, "fg"))
		G.gfx.fg = (unsigned)parse_int(v);
	else if (mmb_keyword_eq(k, "bg"))
		G.gfx.bg = (unsigned)parse_int(v);
	else if (mmb_keyword_eq(k, "search_path"))
	{
		strncpy(G.opt.search_path, v, sizeof(G.opt.search_path) - 1);
		G.opt.search_path[sizeof(G.opt.search_path) - 1] = 0;
	}
	else if (mmb_keyword_eq(k, "app_path"))
	{
		strncpy(G.opt.app_path, v, sizeof(G.opt.app_path) - 1);
		G.opt.app_path[sizeof(G.opt.app_path) - 1] = 0;
	}
	else if (mmb_keyword_eq(k, "boot_mode"))
	{
		G.opt.boot_mode = parse_int(v);
		if (G.opt.boot_mode < 0 || G.opt.boot_mode > 2)
			G.opt.boot_mode = 0;
	}
	else if (mmb_keyword_eq(k, "boot_app"))
	{
		strncpy(G.opt.boot_app, v, sizeof(G.opt.boot_app) - 1);
		G.opt.boot_app[sizeof(G.opt.boot_app) - 1] = 0;
	}
	else if (k[0] == 'f' && k[1] >= '1' && k[1] <= '9' && !k[2])
	{
		i = k[1] - '1';
		strncpy(G.opt.fkey[i], v, 64);
		G.opt.fkey[i][64] = 0;
	}
	else if (k[0] == 'f' && k[1] == '1' && k[2] >= '0' && k[2] <= '2' && !k[3])
	{
		i = 9 + (k[2] - '0');
		if (i >= 10 && i <= 12)
		{
			strncpy(G.opt.fkey[i - 1], v, 64);
			G.opt.fkey[i - 1][64] = 0;
		}
	}
}

static void apply_wifi(const char *k, const char *v)
{
	if (mmb_keyword_eq(k, "ssid"))
	{
		strncpy(G.opt.wifi_ssid, v, sizeof(G.opt.wifi_ssid) - 1);
		G.opt.wifi_ssid[sizeof(G.opt.wifi_ssid) - 1] = 0;
	}
	else if (mmb_keyword_eq(k, "psk"))
	{
		strncpy(G.opt.wifi_psk, v, sizeof(G.opt.wifi_psk) - 1);
		G.opt.wifi_psk[sizeof(G.opt.wifi_psk) - 1] = 0;
	}
	else if (mmb_keyword_eq(k, "enabled"))
		G.opt.wifi_enabled = parse_int(v);
	else if (mmb_keyword_eq(k, "debug"))
		G.opt.wifi_debug = parse_int(v);
	else if (mmb_keyword_eq(k, "country"))
	{
		char cc[3];
		if (mmb_wifi_country_normalize(v, cc))
		{
			G.opt.wifi_country[0] = cc[0];
			G.opt.wifi_country[1] = cc[1];
			G.opt.wifi_country[2] = 0;
		}
	}
}

void mmb_settings_save(void)
{
	char buf[SETTINGS_MAX];
	int i;
	choose_path();
	buf[0] = 0;
	append(buf, sizeof(buf), "[core]\n");
	kv_str(buf, sizeof(buf), "keyboard", keyboard_name(G.opt.keyboard_lang));
	kv_int(buf, sizeof(buf), "keyboard_noled", G.opt.keyboard_noled);
	kv_int(buf, sizeof(buf), "repeat_first", G.opt.repeat_first);
	kv_int(buf, sizeof(buf), "repeat_next", G.opt.repeat_next);
	kv_int(buf, sizeof(buf), "tab", G.opt.tab);
	kv_int(buf, sizeof(buf), "break", G.opt.break_key);
	kv_int(buf, sizeof(buf), "autorun", G.opt.autorun);
	kv_int(buf, sizeof(buf), "colourcode", G.opt.colourcode);
	kv_int(buf, sizeof(buf), "colourcode_reverse", G.opt.colourcode_reverse);
	kv_int(buf, sizeof(buf), "console", G.opt.console);
	kv_int(buf, sizeof(buf), "console_port", G.opt.console_port);
	kv_int(buf, sizeof(buf), "crlf", G.opt.crlf);
	kv_int(buf, sizeof(buf), "default_mode", G.opt.default_mode);
	kv_int(buf, sizeof(buf), "baudrate", G.opt.baudrate);
	kv_int(buf, sizeof(buf), "case", G.opt.case_mode);
	kv_int(buf, sizeof(buf), "legacy", G.opt.legacy);
	kv_int(buf, sizeof(buf), "milliseconds", G.opt.milliseconds);
	kv_int(buf, sizeof(buf), "mouse", G.opt.mouse);
	kv_int(buf, sizeof(buf), "mouse_sens", G.opt.mouse_sens);
	kv_int(buf, sizeof(buf), "pin", G.opt.pin);
	kv_int(buf, sizeof(buf), "profiling", G.opt.profiling);
	kv_int(buf, sizeof(buf), "prompt", G.opt.prompt);
	kv_int(buf, sizeof(buf), "status", G.opt.status);
	append(buf, sizeof(buf), "vcc=");
	{
		int whole = (int)G.opt.vcc;
		int frac = (int)((G.opt.vcc - (double)whole) * 10 + (G.opt.vcc < 0 ? -0.5 : 0.5));
		if (frac < 0)
			frac = -frac;
		append_int(buf, sizeof(buf), whole);
		append(buf, sizeof(buf), ".");
		append_int(buf, sizeof(buf), frac);
		append(buf, sizeof(buf), "\n");
	}
	kv_int(buf, sizeof(buf), "sleep", G.opt.sleep_min);
	kv_int(buf, sizeof(buf), "sd_fast", G.opt.sd_fast);
	kv_int(buf, sizeof(buf), "serial_pullup", G.opt.serial_pullup);
	kv_int(buf, sizeof(buf), "rtc_cal", G.opt.rtc_cal);
	kv_int(buf, sizeof(buf), "ds3231", G.opt.ds3231);
	kv_int(buf, sizeof(buf), "baseline", G.opt.baseline);
	kv_int(buf, sizeof(buf), "edit_font", G.opt.edit_font);
	kv_int(buf, sizeof(buf), "theme", G.opt.theme);
	kv_int(buf, sizeof(buf), "edit_theme", G.opt.edit_theme);
	kv_int(buf, sizeof(buf), "edit_jump_break", G.opt.edit_jump_break);
	kv_int(buf, sizeof(buf), "y_axis_up", G.opt.y_axis_up);
	kv_int(buf, sizeof(buf), "angle_degrees", G.opt.angle_degrees);
	kv_int(buf, sizeof(buf), "error_continue", G.opt.error_continue);
	kv_int(buf, sizeof(buf), "audio_on", G.opt.audio_on);
	kv_int(buf, sizeof(buf), "audio_target", G.opt.audio_target);
	kv_int(buf, sizeof(buf), "term_log", G.opt.term_log);
	kv_int(buf, sizeof(buf), "term_scrollback", G.opt.term_scrollback);
	kv_int(buf, sizeof(buf), "term_autolog", G.opt.term_autolog);
	kv_int(buf, sizeof(buf), "fg", (int64_t)G.gfx.fg);
	kv_int(buf, sizeof(buf), "bg", (int64_t)G.gfx.bg);
	if (G.opt.search_path[0])
		kv_str(buf, sizeof(buf), "search_path", G.opt.search_path);
	kv_str(buf, sizeof(buf), "app_path",
	       G.opt.app_path[0] ? G.opt.app_path : MMB_APP_PATH_DEFAULT);
	kv_int(buf, sizeof(buf), "boot_mode", G.opt.boot_mode);
	if (G.opt.boot_app[0])
		kv_str(buf, sizeof(buf), "boot_app", G.opt.boot_app);
	for (i = 0; i < 12; i++)
	{
		if (G.opt.fkey[i][0])
		{
			char key[4];
			key[0] = 'f';
			if (i < 9)
			{
				key[1] = (char)('1' + i);
				key[2] = 0;
			}
			else
			{
				key[1] = '1';
				key[2] = (char)('0' + (i - 9));
				key[3] = 0;
			}
			kv_str(buf, sizeof(buf), key, G.opt.fkey[i]);
		}
	}
	append(buf, sizeof(buf), "\n[wifi]\n");
	kv_str(buf, sizeof(buf), "ssid", G.opt.wifi_ssid);
	kv_str(buf, sizeof(buf), "psk", G.opt.wifi_psk);
	kv_int(buf, sizeof(buf), "enabled", G.opt.wifi_enabled);
	kv_int(buf, sizeof(buf), "debug", G.opt.wifi_debug);
	kv_str(buf, sizeof(buf), "country", mmb_opt_wifi_country());
	append(buf, sizeof(buf), "\n[ethernet]\n");
	kv_int(buf, sizeof(buf), "enabled", G.opt.ethernet_enabled);
	append(buf, sizeof(buf), "\n[ntp]\n");
	kv_int(buf, sizeof(buf), "enabled", G.opt.ntp_enabled);
	kv_str(buf, sizeof(buf), "server", G.opt.ntp_server);
	kv_str(buf, sizeof(buf), "timezone", G.opt.timezone);
	mmb_vfs_write(settings_path, buf, (unsigned)strlen(buf), 0);
}

void mmb_settings_load(void)
{
	char buf[SETTINGS_MAX];
	unsigned got = 0;
	char *p, *nl;
	int section = 0; /* 1 core 2 wifi 3 ethernet 4 ntp */

	saw_theme_key = 0;
	saw_edit_theme_key = 0;
	choose_path();
	if (!mmb_vfs_exists(settings_path))
		return;
	if (mmb_vfs_read(settings_path, buf, sizeof(buf) - 1, &got) != 0)
		return;
	buf[got] = 0;
	p = buf;
	while (*p)
	{
		nl = p;
		while (*nl && *nl != '\n')
			nl++;
		if (*nl)
			*nl++ = 0;
		if (p[0] && p[strlen(p) - 1] == '\r')
			p[strlen(p) - 1] = 0;
		trim(p);
		if (p[0] == 0 || p[0] == '#' || p[0] == ';')
		{
			p = nl;
			continue;
		}
		if (p[0] == '[')
		{
			if (mmb_keyword_eq(p, "[core]"))
				section = 1;
			else if (mmb_keyword_eq(p, "[wifi]"))
				section = 2;
			else if (mmb_keyword_eq(p, "[ethernet]"))
				section = 3;
			else if (mmb_keyword_eq(p, "[ntp]"))
				section = 4;
			else
				section = 0;
			p = nl;
			continue;
		}
		{
			char *eq = p;
			while (*eq && *eq != '=')
				eq++;
			if (*eq == '=')
			{
				*eq++ = 0;
				trim(p);
				trim(eq);
				if (section == 1)
					apply_core(p, eq);
				else if (section == 2)
					apply_wifi(p, eq);
				else if (section == 3)
				{
					if (mmb_keyword_eq(p, "enabled"))
						G.opt.ethernet_enabled = parse_int(eq);
				}
				else if (section == 4)
				{
					if (mmb_keyword_eq(p, "enabled"))
						G.opt.ntp_enabled = parse_int(eq);
					else if (mmb_keyword_eq(p, "server") && eq[0])
					{
						strncpy(G.opt.ntp_server, eq,
							sizeof(G.opt.ntp_server) - 1);
						G.opt.ntp_server[sizeof(G.opt.ntp_server) - 1] = 0;
					}
					else if (mmb_keyword_eq(p, "timezone"))
					{
						int off = 0;
						char canon[64];

						if (mmb_timezone_normalize(eq, &off, canon,
									   (int)sizeof(canon)))
						{
							strncpy(G.opt.timezone, canon,
								sizeof(G.opt.timezone) - 1);
							G.opt.timezone[sizeof(G.opt.timezone) - 1] = 0;
							G.opt.tz_offset_min = off;
						}
					}
				}
			}
		}
		p = nl;
	}
	/* Pre-split configs stored the single shared theme in edit_theme. Treat
	 * that as the system theme and leave the editor following it. */
	if (!saw_theme_key && saw_edit_theme_key)
	{
		if (G.opt.edit_theme >= 0 && G.opt.edit_theme < 10)
			G.opt.theme = G.opt.edit_theme;
		G.opt.edit_theme = MMB_OPT_EDIT_THEME_SYSTEM;
	}
	if (G.opt.theme < 0 || G.opt.theme >= 10)
		G.opt.theme = MMB_OPT_DEFAULT_THEME;
	if (G.opt.edit_theme < MMB_OPT_EDIT_THEME_SYSTEM || G.opt.edit_theme >= 10)
		G.opt.edit_theme = MMB_OPT_EDIT_THEME_SYSTEM;
	if (G.opt.ethernet_enabled)
		G.opt.wifi_enabled = 0;
}

void mmb_cmd_factory_reset(void)
{
	mmb_option_reset();
	G.gfx.fg = MMB_DEFAULT_FG;
	G.gfx.bg = MMB_DEFAULT_BG;
	G.opt.wifi_ssid[0] = 0;
	G.opt.wifi_psk[0] = 0;
	G.opt.wifi_enabled = 0;
	G.opt.wifi_debug = 0;
	G.opt.ethernet_enabled = 0;
	G.opt.ntp_enabled = MMB_NTP_AUTO;
	strncpy(G.opt.ntp_server, MMB_NTP_DEFAULT_SERVER, sizeof(G.opt.ntp_server) - 1);
	G.opt.ntp_server[sizeof(G.opt.ntp_server) - 1] = 0;
	strncpy(G.opt.timezone, "UTC", sizeof(G.opt.timezone) - 1);
	G.opt.timezone[sizeof(G.opt.timezone) - 1] = 0;
	G.opt.tz_offset_min = 0;
	G.opt.term_log = 0;
	G.opt.term_scrollback = 200;
	G.opt.term_autolog = 0;
	mmb_audio_apply_options();
	mmb_console_apply_colour();
	mmb_gfx_apply_default_mode();
	mmb_settings_save();
	mmb_out("Factory defaults restored");
}
