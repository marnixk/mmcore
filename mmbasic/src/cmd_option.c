#include "mmb_priv.h"

static int onoff(void)
{
	mmb_skip_sp();
	if (mmb_match("ON"))
		return 1;
	if (mmb_match("OFF"))
		return 0;
	mmb_syntax();
	return 0;
}

static void set_fkey(int n)
{
	mmb_val v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	if (n < 1 || n > 12)
		mmb_syntax();
	strncpy(G.opt.fkey[n - 1], v.s, 64);
	G.opt.fkey[n - 1][64] = 0;
}

/* Consume remaining tokens on a hardware OPTION line (parse-only options). */
static void skip_hw_rest(void)
{
	for (;;)
	{
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
			return;
		if (*G.p == '"')
		{
			mmb_expr();
		}
		else if (*G.p == ',' || *G.p == ' ')
		{
			G.p++;
		}
		else if (mmb_is_digit(*G.p) || *G.p == '-' || *G.p == '+')
		{
			mmb_expr();
		}
		else if ((unsigned char)*G.p == 0x80)
			G.p += 3;
		else if (mmb_is_ident(*G.p) || *G.p == 'x' || *G.p == 'X')
		{
			while (mmb_is_ident(*G.p) || *G.p == 'x' || *G.p == 'X')
				G.p++;
		}
		else
			G.p++;
	}
}

static int parse_audio_target_name(void)
{
	if (mmb_match("HDMI") || mmb_match("HDMI0") || mmb_match("TV") ||
	    mmb_match("MONITOR"))
		return 1;
	if (mmb_match("JACK") || mmb_match("HEADPHONE") ||
	    mmb_match("HEADPHONES") || mmb_match("PHONES") ||
	    mmb_match("PWM") || mmb_match("ANALOG") || mmb_match("ANALOGUE"))
		return 0;
	mmb_syntax();
	return 1;
}

static void parse_audio_target(void)
{
	G.opt.audio_target = parse_audio_target_name();
}

static void parse_audio(void)
{
	mmb_skip_sp();
	if (mmb_match("TARGET"))
	{
		parse_audio_target();
		return;
	}
	if (mmb_match("ON"))
	{
		G.opt.audio_on = 1;
		return;
	}
	if (mmb_match("OFF"))
	{
		G.opt.audio_on = 0;
		return;
	}
	skip_hw_rest();
}

static void parse_display(void)
{
	mmb_skip_sp();
	if (mmb_match("DISABLE"))
		return;
	if (mmb_is_digit(*G.p) || *G.p == '-')
	{
		mmb_expr();
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			mmb_expr();
		}
		return;
	}
	skip_hw_rest();
}

static void parse_lcdpanel(void)
{
	skip_hw_rest();
}

static void parse_touch(void)
{
	mmb_skip_sp();
	if (mmb_match("DISABLE"))
		return;
	skip_hw_rest();
}

static void cons_write(const char *s)
{
	mmb_console_write(s);
}

static int read_console_line(char *buf, unsigned maxn, int hide)
{
	if (G.plat && G.plat->read_line)
		return G.plat->read_line(buf, maxn, hide);
	return -1;
}

static void wifi_store(const char *ssid, const char *psk)
{
	strncpy(G.opt.wifi_ssid, ssid ? ssid : "", sizeof(G.opt.wifi_ssid) - 1);
	G.opt.wifi_ssid[sizeof(G.opt.wifi_ssid) - 1] = 0;
	strncpy(G.opt.wifi_psk, psk ? psk : "", sizeof(G.opt.wifi_psk) - 1);
	G.opt.wifi_psk[sizeof(G.opt.wifi_psk) - 1] = 0;
	G.opt.wifi_enabled = G.opt.wifi_ssid[0] ? 1 : 0;
	mmb_settings_save();
}

static void wifi_try_connect(const char *ssid, const char *psk, int saved_now)
{
	if (mmb_wlan_connect(ssid, psk) == 0)
	{
		char ip[40];
		char msg[192];
		int n = 0;

		ip[0] = 0;
		if (mmb_wlan_ip(ip, (int)sizeof(ip)) == 0 && ip[0] && ssid && ssid[0])
		{
			const char *a = "Connected to '";
			const char *b = "' as ";
			while (*a && n < (int)sizeof(msg) - 1)
				msg[n++] = *a++;
			while (*ssid && n < (int)sizeof(msg) - 1)
				msg[n++] = *ssid++;
			while (*b && n < (int)sizeof(msg) - 1)
				msg[n++] = *b++;
			a = ip;
			while (*a && n < (int)sizeof(msg) - 1)
				msg[n++] = *a++;
			msg[n] = 0;
			mmb_out(msg);
		}
		else
			mmb_out("Wi-Fi connected");
	}
	else if (!mmb_wlan_available())
		mmb_out(saved_now
			? "Wi-Fi credentials saved (radio not available)"
			: "Wi-Fi not available");
	else
		mmb_out("Wi-Fi connect failed");
}

static void wifi_connect_stored(void)
{
	if (!G.opt.wifi_ssid[0])
		mmb_error("?WIFI not configured");
	wifi_try_connect(G.opt.wifi_ssid, G.opt.wifi_psk, 0);
}

static void parse_wifi_interactive(void)
{
	char ssids[16][64];
	char line[80];
	char psk[64];
	int n, i, pick;
	if (!mmb_wlan_available())
	{
		mmb_out("Wi-Fi not available");
		return;
	}
	n = mmb_wlan_scan(ssids, 16);
	if (n <= 0)
		cons_write("\nNo networks found\r\nSSID: ");
	else
	{
		cons_write("\nWi-Fi networks:\r\n");
		for (i = 0; i < n; i++)
		{
			char num[8];
			num[0] = (char)('1' + i);
			if (i >= 9)
			{
				num[0] = (char)('0' + (i + 1) / 10);
				num[1] = (char)('0' + (i + 1) % 10);
				num[2] = 0;
			}
			else
			{
				num[1] = 0;
			}
			cons_write(num);
			cons_write(". ");
			cons_write(ssids[i]);
			cons_write("\r\n");
		}
		cons_write("SSID or number: ");
	}
	if (read_console_line(line, sizeof(line), 0) != 0)
	{
		mmb_out("Wi-Fi not available");
		return;
	}
	pick = 0;
	if (line[0] >= '1' && line[0] <= '9' && (line[1] == 0 || (line[1] >= '0' && line[1] <= '9' && line[2] == 0)))
	{
		pick = line[0] - '0';
		if (line[1])
			pick = pick * 10 + (line[1] - '0');
		if (pick < 1 || pick > n)
			pick = 0;
		else
			strncpy(line, ssids[pick - 1], sizeof(line) - 1);
	}
	if (!line[0])
	{
		mmb_out("Wi-Fi cancelled");
		return;
	}
	cons_write("Password: ");
	if (read_console_line(psk, sizeof(psk), 1) != 0)
		psk[0] = 0;
	wifi_store(line, psk);
	wifi_try_connect(line, psk, 1);
}

static int wifi_country_set(const char *s)
{
	char cc[3];
	if (!mmb_wifi_country_normalize(s, cc))
		return 0;
	G.opt.wifi_country[0] = cc[0];
	G.opt.wifi_country[1] = cc[1];
	G.opt.wifi_country[2] = 0;
	return 1;
}

static void parse_wifi(void)
{
	mmb_skip_sp();
	if (mmb_match("DEBUG"))
	{
		G.opt.wifi_debug = onoff();
		mmb_settings_save();
		return;
	}
	if (mmb_match("COUNTRY"))
	{
		mmb_val v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		if (!wifi_country_set(v.s))
			mmb_error("?SYNTAX ERROR");
		mmb_settings_save();
		mmb_wlan_apply_country();
		return;
	}
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
	{
		parse_wifi_interactive();
		return;
	}
	{
		mmb_val ssid = mmb_expr();
		mmb_val psk;
		if (ssid.type != T_STR)
			mmb_syntax();
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
			psk = mmb_str_val("");
		else
			psk = mmb_expr();
		if (psk.type != T_STR)
			mmb_syntax();
		wifi_store(ssid.s, psk.s);
		wifi_try_connect(ssid.s, psk.s, 1);
	}
}

static void parse_sdcard(void)
{
	skip_hw_rest();
}

static void parse_resolution(void)
{
	skip_hw_rest();
}

static void parse_clock(void)
{
	skip_hw_rest();
}

static void option_dispatch(void)
{
	if (mmb_match("BASE"))
	{
		mmb_val v;
		if (G.dim_used)
			mmb_error("?MUST BE BEFORE DIM");
		v = mmb_expr();
		{
			int b = (int)mmb_as_int(v);
			if (b != 0 && b != 1)
				mmb_syntax();
			G.opt.base = b;
		}
		return;
	}
	if (mmb_match("EXPLICIT"))
	{
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || mmb_match("ON"))
			G.opt.explicit = 1;
		else if (mmb_match("OFF"))
			G.opt.explicit = 0;
		else
			G.opt.explicit = 1;
		return;
	}
	if (mmb_match("DEFAULT"))
	{
		if (mmb_match("MODE"))
		{
			G.opt.default_mode = (int)mmb_as_int(mmb_expr());
			mmb_gfx_apply_default_mode();
			return;
		}
		if (mmb_match("COLOURS") || mmb_match("COLORS"))
		{
			G.gfx.fg = mmb_colour_from_int(mmb_as_int(mmb_expr()));
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				G.gfx.bg = mmb_colour_from_int(mmb_as_int(mmb_expr()));
			}
			mmb_console_apply_colour();
			return;
		}
		if (mmb_match("INTEGER"))
			G.opt.default_type = T_INT;
		else if (mmb_match("FLOAT"))
			G.opt.default_type = T_NUM;
		else if (mmb_match("STRING"))
			G.opt.default_type = T_STR;
		else if (mmb_match("NONE"))
			G.opt.default_type = 0;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("ANGLE"))
	{
		if (mmb_match("DEGREES"))
			G.opt.angle_degrees = 1;
		else if (mmb_match("RADIANS"))
			G.opt.angle_degrees = 0;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("Y_AXIS"))
	{
		if (mmb_match("UP"))
			G.opt.y_axis_up = 1;
		else if (mmb_match("DOWN"))
			G.opt.y_axis_up = 0;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("TAB"))
	{
		int t = (int)mmb_as_int(mmb_expr());
		if (t != 2 && t != 3 && t != 4 && t != 8)
			mmb_syntax();
		G.opt.tab = t;
		return;
	}
	if (mmb_match("TERM"))
	{
		if (!mmb_match("LOG"))
			mmb_syntax();
		G.opt.term_log = onoff();
		mmb_term_log_enable(G.opt.term_log);
		return;
	}
	if (mmb_match("BREAK"))
	{
		G.opt.break_key = (int)mmb_as_int(mmb_expr());
		return;
	}
	if (mmb_match("AUTORUN"))
	{
		G.opt.autorun = onoff();
		return;
	}
	if (mmb_match("COLOURCODE") || mmb_match("COLORCODE"))
	{
		if (mmb_match("REVERSE"))
		{
			G.opt.colourcode_reverse = 1;
			G.opt.colourcode = 0;
		}
		else if (mmb_match("ON"))
		{
			G.opt.colourcode = 1;
			G.opt.colourcode_reverse = 0;
		}
		else if (mmb_match("OFF"))
		{
			G.opt.colourcode = 0;
			G.opt.colourcode_reverse = 0;
		}
		else
			G.opt.colourcode = 1;
		return;
	}
	if (mmb_match("CONSOLE"))
	{
		if (mmb_match("SAVE"))
		{
			G.opt.console_saved = G.opt.console;
			return;
		}
		if (mmb_match("PORT"))
		{
			G.opt.console_port = (int)mmb_as_int(mmb_expr());
			return;
		}
		if (mmb_match("SCREEN"))
			G.opt.console = 2;
		else if (mmb_match("SERIAL"))
			G.opt.console = 1;
		else if (mmb_match("BOTH"))
			G.opt.console = 3;
		else if (mmb_match("NONE"))
			G.opt.console = 0;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("CRLF"))
	{
		if (mmb_match("CRLF"))
			G.opt.crlf = 2;
		else if (mmb_match("CR"))
			G.opt.crlf = 0;
		else if (mmb_match("LF"))
			G.opt.crlf = 1;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("BAUDRATE"))
	{
		G.opt.baudrate = (int)mmb_as_int(mmb_expr());
		return;
	}
	if (mmb_match("CASE"))
	{
		if (mmb_match("UPPER"))
			G.opt.case_mode = 0;
		else if (mmb_match("LOWER"))
			G.opt.case_mode = 1;
		else if (mmb_match("TITLE"))
			G.opt.case_mode = 2;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("LEGACY"))
	{
		G.opt.legacy = onoff();
		return;
	}
	if (mmb_match("MILLISECONDS"))
	{
		G.opt.milliseconds = onoff();
		return;
	}
	if (mmb_match("MOUSE"))
	{
		if (mmb_match("OFF"))
		{
			G.opt.mouse = 0;
			return;
		}
		G.opt.mouse = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			G.opt.mouse_sens = (int)mmb_as_int(mmb_expr());
		}
		return;
	}
	if (mmb_match("PIN"))
	{
		G.opt.pin = (int)mmb_as_int(mmb_expr());
		return;
	}
	if (mmb_match("PROMPT"))
	{
		if (mmb_match("BARE"))
			G.opt.prompt = 0;
		else if (mmb_match("CWD"))
			G.opt.prompt = 1;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("PROFILING"))
	{
		G.opt.profiling = onoff();
		return;
	}
	if (mmb_match("TRACECACHE"))
	{
		G.opt.tracecache = onoff();
		if (G.opt.tracecache == 0)
			mmb_tcache_invalidate();
		mmb_skip_sp();
		if (*G.p && *G.p != ':' && *G.p != '\'')
			mmb_expr();
		return;
	}
	if (mmb_match("RAM"))
	{
		G.opt.ram_prog = 1;
		return;
	}
	if (mmb_match("FLASH"))
	{
		G.opt.ram_prog = 0;
		mmb_skip_sp();
		if (*G.p && *G.p != ':' && *G.p != '\'')
			G.opt.flash_page = (int)mmb_as_int(mmb_expr());
		return;
	}
	if (mmb_match("STATUS"))
	{
		G.opt.status = onoff();
		return;
	}
	if (mmb_match("VCC"))
	{
		G.opt.vcc = mmb_as_float(mmb_expr());
		return;
	}
	if (mmb_match("SLEEP"))
	{
		G.opt.sleep_min = (int)mmb_as_int(mmb_expr());
		return;
	}
	if (mmb_match("SD"))
	{
		if (!mmb_match("TIMING"))
			mmb_syntax();
		if (mmb_match("FAST"))
			G.opt.sd_fast = 1;
		else if (mmb_match("NORMAL"))
			G.opt.sd_fast = 0;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("SERIAL"))
	{
		if (!mmb_match("PULLUP"))
			mmb_syntax();
		if (mmb_match("ENABLE"))
			G.opt.serial_pullup = 1;
		else if (mmb_match("DISABLE"))
			G.opt.serial_pullup = 0;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("RTC"))
	{
		if (!mmb_match("CALIBRATE"))
			mmb_syntax();
		G.opt.rtc_cal = (int)mmb_as_int(mmb_expr());
		return;
	}
	if (mmb_match("DS3231"))
	{
		G.opt.ds3231 = onoff();
		return;
	}
	if (mmb_match("BASELINE"))
	{
		G.opt.baseline = onoff();
		return;
	}
	if (mmb_match("USBKEYBOARD") || mmb_match("KEYBOARD"))
	{
		if (mmb_match("REPEAT"))
		{
			G.opt.repeat_first = (int)mmb_as_int(mmb_expr());
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				G.opt.repeat_next = (int)mmb_as_int(mmb_expr());
			}
			return;
		}
		{
			char lang[8];
			int i = 0;
			mmb_skip_sp();
			while (mmb_is_ident(*G.p) && i < 7)
				lang[i++] = *G.p++;
			lang[i] = 0;
			if (lang[0])
			{
				mmb_upper(lang);
				if (mmb_keyword_eq(lang, "US"))
					G.opt.keyboard_lang = 0;
				else if (mmb_keyword_eq(lang, "UK"))
					G.opt.keyboard_lang = 1;
				else if (mmb_keyword_eq(lang, "DE"))
					G.opt.keyboard_lang = 2;
				else if (mmb_keyword_eq(lang, "FR"))
					G.opt.keyboard_lang = 3;
				else if (mmb_keyword_eq(lang, "ES"))
					G.opt.keyboard_lang = 4;
			}
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				if (mmb_match("NOLED"))
					G.opt.keyboard_noled = 1;
				else
					G.opt.keyboard_noled = (int)mmb_as_int(mmb_expr());
			}
			return;
		}
	}
	if (mmb_match("EDIT"))
	{
		if (mmb_match("THEME"))
		{
			int id = -1;
			mmb_skip_sp();
			if (*G.p == '"')
			{
				mmb_val v = mmb_expr();
				if (v.type != T_STR)
					mmb_syntax();
				id = mmb_editor_theme_lookup(v.s);
			}
			else
			{
				int i, n = mmb_editor_theme_count();
				for (i = 0; i < n; i++)
				{
					if (mmb_match(mmb_editor_theme_name(i)))
					{
						id = i;
						break;
					}
				}
				if (id < 0)
					id = (int)mmb_as_int(mmb_expr());
			}
			if (id < 0 || id >= mmb_editor_theme_count())
				mmb_error("?THEME");
			G.opt.edit_theme = id;
			return;
		}
		if (!mmb_match("FONT"))
			mmb_syntax();
		if (mmb_match("SMALL"))
			G.opt.edit_font = 0;
		else if (mmb_match("NORMAL"))
			G.opt.edit_font = 1;
		else if (mmb_match("MEDIUM"))
			G.opt.edit_font = 2;
		else if (mmb_match("LARGE"))
			G.opt.edit_font = 3;
		else if (mmb_match("VERY"))
		{
			mmb_match("LARGE");
			G.opt.edit_font = 4;
		}
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("ESCAPE"))
	{
		G.opt.escape = 1;
		return;
	}
	if (mmb_match("SEARCH"))
	{
		if (!mmb_match("PATH"))
			mmb_syntax();
		{
			mmb_val v = mmb_expr();
			if (v.type != T_STR)
				mmb_syntax();
			strncpy(G.opt.search_path, v.s, sizeof(G.opt.search_path) - 1);
			G.opt.search_path[sizeof(G.opt.search_path) - 1] = 0;
		}
		return;
	}
	if (mmb_match("ERROR"))
	{
		if (mmb_match("CONTINUE"))
			G.opt.error_continue = 1;
		else if (mmb_match("ABORT"))
			G.opt.error_continue = 0;
		else
			mmb_syntax();
		return;
	}
	if (mmb_match("AUDIO_TARGET"))
	{
		parse_audio_target();
		return;
	}
	if (mmb_match("AUDIO"))
	{
		parse_audio();
		return;
	}
	if (mmb_match("DISPLAY"))
	{
		parse_display();
		return;
	}
	if (mmb_match("LCDPANEL"))
	{
		parse_lcdpanel();
		return;
	}
	if (mmb_match("TOUCH"))
	{
		parse_touch();
		return;
	}
	if (mmb_match("WIFI"))
	{
		parse_wifi();
		return;
	}
	if (mmb_match("CPUSPEED"))
	{
		mmb_expr();
		skip_hw_rest();
		return;
	}
	if (mmb_match("SDCARD"))
	{
		parse_sdcard();
		return;
	}
	if (mmb_match("HEARTBEAT"))
	{
		onoff();
		return;
	}
	if (mmb_match("RESOLUTION"))
	{
		parse_resolution();
		return;
	}
	if (mmb_match("CLOCK"))
	{
		parse_clock();
		return;
	}
	if (mmb_match("F1")) { set_fkey(1); return; }
	if (mmb_match("F2")) { set_fkey(2); return; }
	if (mmb_match("F3")) { set_fkey(3); return; }
	if (mmb_match("F4")) { set_fkey(4); return; }
	if (mmb_match("F5")) { set_fkey(5); return; }
	if (mmb_match("F6")) { set_fkey(6); return; }
	if (mmb_match("F7")) { set_fkey(7); return; }
	if (mmb_match("F8")) { set_fkey(8); return; }
	if (mmb_match("F9")) { set_fkey(9); return; }
	if (mmb_match("F10")) { set_fkey(10); return; }
	if (mmb_match("F11")) { set_fkey(11); return; }
	if (mmb_match("F12")) { set_fkey(12); return; }
	if (mmb_match("RESET"))
	{
		char ssid[64], psk[64];
		int en = G.opt.wifi_enabled;
		strncpy(ssid, G.opt.wifi_ssid, sizeof(ssid) - 1);
		ssid[sizeof(ssid) - 1] = 0;
		strncpy(psk, G.opt.wifi_psk, sizeof(psk) - 1);
		psk[sizeof(psk) - 1] = 0;
		mmb_option_reset();
		strncpy(G.opt.wifi_ssid, ssid, sizeof(G.opt.wifi_ssid) - 1);
		G.opt.wifi_ssid[sizeof(G.opt.wifi_ssid) - 1] = 0;
		strncpy(G.opt.wifi_psk, psk, sizeof(G.opt.wifi_psk) - 1);
		G.opt.wifi_psk[sizeof(G.opt.wifi_psk) - 1] = 0;
		G.opt.wifi_enabled = en;
		mmb_gfx_apply_default_mode();
		return;
	}
	if (mmb_match("LIST"))
	{
		int all = 0;
		if (mmb_match("ALL"))
			all = 1;
		mmb_option_list(all);
		return;
	}
	/* Pico-flavoured aliases accepted and stored where possible */
	if (mmb_match("NOCHECK") || mmb_match("LOGGING") || mmb_match("FAST"))
	{
		onoff();
		return;
	}
	mmb_syntax();
}

void mmb_cmd_options(void)
{
	mmb_skip_sp();
	if (!mmb_match("WIFI"))
		mmb_syntax();
	mmb_skip_sp();
	if (*G.p != 0 && *G.p != ':' && *G.p != '\'')
		mmb_syntax();
	wifi_connect_stored();
}

void mmb_cmd_option(void)
{
	const char *save = G.p;
	mmb_skip_sp();
	if (mmb_match("LIST"))
	{
		G.p = save;
		option_dispatch();
		return;
	}
	if (mmb_match("WIFI"))
	{
		G.p = save;
		option_dispatch();
		return;
	}
	G.p = save;
	option_dispatch();
	mmb_audio_apply_options();
	mmb_settings_save();
}

static void ol_line(int *n, const char *text)
{
	if (*n)
		mmb_out("\n");
	mmb_out(text);
	(*n)++;
}

static void ol_line_int(int *n, const char *prefix, int64_t val)
{
	if (*n)
		mmb_out("\n");
	mmb_out(prefix);
	mmb_outf(0, val);
	(*n)++;
}

static const char *edit_font_name(int f)
{
	switch (f)
	{
	case 0: return "SMALL";
	case 1: return "NORMAL";
	case 2: return "MEDIUM";
	case 3: return "LARGE";
	default: return "VERY LARGE";
	}
}

static const char *keyboard_lang_name(int lang)
{
	switch (lang)
	{
	case 0: return "US";
	case 1: return "UK";
	case 2: return "DE";
	case 3: return "FR";
	default: return "ES";
	}
}

void mmb_option_list(int all)
{
	int n = 0;
	int i;

	if (all || G.opt.base)
		ol_line(&n, G.opt.base ? "OPTION BASE 1" : "OPTION BASE 0");
	if (all || G.opt.explicit)
		ol_line(&n, "OPTION EXPLICIT");
	if (all)
	{
		if (G.opt.default_type == T_NUM)
			ol_line(&n, "OPTION DEFAULT FLOAT");
		else if (G.opt.default_type == T_INT)
			ol_line(&n, "OPTION DEFAULT INTEGER");
		else if (G.opt.default_type == T_STR)
			ol_line(&n, "OPTION DEFAULT STRING");
		else
			ol_line(&n, "OPTION DEFAULT NONE");
	}
	else
	{
		if (G.opt.default_type == T_INT)
			ol_line(&n, "OPTION DEFAULT INTEGER");
		else if (G.opt.default_type == T_STR)
			ol_line(&n, "OPTION DEFAULT STRING");
		else if (G.opt.default_type == 0)
			ol_line(&n, "OPTION DEFAULT NONE");
	}
	if (all || G.opt.default_mode != MMB_OPT_DEFAULT_MODE)
		ol_line_int(&n, "OPTION DEFAULT MODE ", G.opt.default_mode);
	if (all || G.gfx.fg != 0x808080u || G.gfx.bg != 0)
	{
		if (n)
			mmb_out("\n");
		mmb_out("OPTION DEFAULT COLOURS ");
		mmb_outf(0, (int64_t)G.gfx.fg);
		mmb_out(",");
		mmb_outf(0, (int64_t)G.gfx.bg);
		n++;
	}
	if (all || G.opt.angle_degrees)
		ol_line(&n, G.opt.angle_degrees ? "OPTION ANGLE DEGREES" : "OPTION ANGLE RADIANS");
	if (all || G.opt.y_axis_up)
		ol_line(&n, G.opt.y_axis_up ? "OPTION Y_AXIS UP" : "OPTION Y_AXIS DOWN");
	if (all || G.opt.tab != 2)
		ol_line_int(&n, "OPTION TAB ", G.opt.tab);
	if (all || G.opt.break_key != 3)
		ol_line_int(&n, "OPTION BREAK ", G.opt.break_key);
	if (all || G.opt.autorun)
		ol_line(&n, G.opt.autorun ? "OPTION AUTORUN ON" : "OPTION AUTORUN OFF");
	if (all || !G.opt.colourcode || G.opt.colourcode_reverse)
	{
		if (G.opt.colourcode_reverse)
			ol_line(&n, "OPTION COLOURCODE REVERSE");
		else if (G.opt.colourcode)
			ol_line(&n, "OPTION COLOURCODE ON");
		else
			ol_line(&n, "OPTION COLOURCODE OFF");
	}
	if (all || G.opt.console != MMB_DEFAULT_CONSOLE || G.opt.console_port != 3)
	{
		if (G.opt.console == 2)
			ol_line(&n, "OPTION CONSOLE SCREEN");
		else if (G.opt.console == 1)
			ol_line(&n, "OPTION CONSOLE SERIAL");
		else if (G.opt.console == 3)
			ol_line(&n, "OPTION CONSOLE BOTH");
		else
			ol_line(&n, "OPTION CONSOLE NONE");
		if (all || G.opt.console_port != 3)
			ol_line_int(&n, "OPTION CONSOLE PORT ", G.opt.console_port);
	}
	if (all || G.opt.crlf != 2)
	{
		if (G.opt.crlf == 0)
			ol_line(&n, "OPTION CRLF CR");
		else if (G.opt.crlf == 1)
			ol_line(&n, "OPTION CRLF LF");
		else
			ol_line(&n, "OPTION CRLF CRLF");
	}
	if (all || G.opt.baudrate != 115200)
		ol_line_int(&n, "OPTION BAUDRATE ", G.opt.baudrate);
	if (all || G.opt.case_mode)
	{
		if (G.opt.case_mode == 1)
			ol_line(&n, "OPTION CASE LOWER");
		else if (G.opt.case_mode == 2)
			ol_line(&n, "OPTION CASE TITLE");
		else
			ol_line(&n, "OPTION CASE UPPER");
	}
	if (all || G.opt.legacy)
		ol_line(&n, G.opt.legacy ? "OPTION LEGACY ON" : "OPTION LEGACY OFF");
	if (all || G.opt.milliseconds)
		ol_line(&n, G.opt.milliseconds ? "OPTION MILLISECONDS ON" : "OPTION MILLISECONDS OFF");
	if (all || G.opt.mouse || G.opt.mouse_sens != 1)
	{
		if (G.opt.mouse == 0 && all)
			ol_line(&n, "OPTION MOUSE OFF");
		else if (G.opt.mouse)
		{
			if (n)
				mmb_out("\n");
			mmb_out("OPTION MOUSE ");
			mmb_outf(0, G.opt.mouse);
			if (all || G.opt.mouse_sens != 1)
			{
				mmb_out(",");
				mmb_outf(0, G.opt.mouse_sens);
			}
			n++;
		}
	}
	if (all || G.opt.pin)
		ol_line_int(&n, "OPTION PIN ", G.opt.pin);
	if (all || G.opt.prompt != MMB_OPT_DEFAULT_PROMPT)
		ol_line(&n, G.opt.prompt ? "OPTION PROMPT CWD" : "OPTION PROMPT BARE");
	if (all || G.opt.profiling)
		ol_line(&n, G.opt.profiling ? "OPTION PROFILING ON" : "OPTION PROFILING OFF");
	if (all || !G.opt.tracecache)
		ol_line(&n, G.opt.tracecache ? "OPTION TRACECACHE ON" : "OPTION TRACECACHE OFF");
	if (all || G.opt.ram_prog || (!G.opt.ram_prog && G.opt.flash_page))
	{
		if (G.opt.ram_prog)
			ol_line(&n, "OPTION RAM");
		else if (G.opt.flash_page)
			ol_line_int(&n, "OPTION FLASH ", G.opt.flash_page);
		else if (all)
			ol_line(&n, "OPTION FLASH");
	}
	if (all || !G.opt.status)
		ol_line(&n, G.opt.status ? "OPTION STATUS ON" : "OPTION STATUS OFF");
	if (all || G.opt.vcc != 3.3)
	{
		int whole, frac;
		if (n)
			mmb_out("\n");
		mmb_out("OPTION VCC ");
		if (G.opt.vcc < 0)
		{
			mmb_out("-");
			whole = (int)(-G.opt.vcc);
			frac = (int)((-G.opt.vcc - whole) * 10 + 0.5);
		}
		else
		{
			whole = (int)G.opt.vcc;
			frac = (int)((G.opt.vcc - whole) * 10 + 0.5);
		}
		mmb_outf(0, whole);
		if (frac)
		{
			mmb_out(".");
			mmb_outf(0, frac);
		}
		n++;
	}
	if (all || G.opt.sleep_min)
		ol_line_int(&n, "OPTION SLEEP ", G.opt.sleep_min);
	if (all || G.opt.sd_fast)
		ol_line(&n, G.opt.sd_fast ? "OPTION SD TIMING FAST" : "OPTION SD TIMING NORMAL");
	if (all || G.opt.serial_pullup)
		ol_line(&n, G.opt.serial_pullup ? "OPTION SERIAL PULLUP ENABLE" : "OPTION SERIAL PULLUP DISABLE");
	if (all || G.opt.rtc_cal)
		ol_line_int(&n, "OPTION RTC CALIBRATE ", G.opt.rtc_cal);
	if (all || G.opt.ds3231)
		ol_line(&n, G.opt.ds3231 ? "OPTION DS3231 ON" : "OPTION DS3231 OFF");
	if (all || G.opt.baseline)
		ol_line(&n, G.opt.baseline ? "OPTION BASELINE ON" : "OPTION BASELINE OFF");
	if (all || G.opt.repeat_first != MMB_REPEAT_FIRST_DEFAULT ||
	    G.opt.repeat_next != MMB_REPEAT_NEXT_DEFAULT)
	{
		if (n)
			mmb_out("\n");
		mmb_out("OPTION KEYBOARD REPEAT ");
		mmb_outf(0, G.opt.repeat_first);
		mmb_out(",");
		mmb_outf(0, G.opt.repeat_next);
		n++;
	}
	if (all || G.opt.keyboard_lang || G.opt.keyboard_noled)
	{
		if (n)
			mmb_out("\n");
		mmb_out("OPTION USBKEYBOARD ");
		mmb_out(keyboard_lang_name(G.opt.keyboard_lang));
		if (G.opt.keyboard_noled)
		{
			mmb_out(",NOLED");
		}
		n++;
	}
	if (all || G.opt.edit_font != 1)
	{
		if (n)
			mmb_out("\n");
		mmb_out("OPTION EDIT FONT ");
		mmb_out(edit_font_name(G.opt.edit_font));
		n++;
	}
	if (all || G.opt.edit_theme != MMB_OPT_DEFAULT_EDIT_THEME)
	{
		if (n)
			mmb_out("\n");
		mmb_out("OPTION EDIT THEME ");
		mmb_out(mmb_editor_theme_name(G.opt.edit_theme));
		n++;
	}
	if (all || G.opt.escape)
		ol_line(&n, "OPTION ESCAPE");
	if (all || G.opt.error_continue)
		ol_line(&n, G.opt.error_continue ? "OPTION ERROR CONTINUE" : "OPTION ERROR ABORT");
	if (all || G.opt.audio_target != 1)
		ol_line(&n, G.opt.audio_target ? "OPTION AUDIO_TARGET HDMI" : "OPTION AUDIO_TARGET JACK");
	if (all || !G.opt.audio_on)
		ol_line(&n, G.opt.audio_on ? "OPTION AUDIO ON" : "OPTION AUDIO OFF");
	if (all || G.opt.wifi_debug)
		ol_line(&n, G.opt.wifi_debug ? "OPTION WIFI DEBUG ON" : "OPTION WIFI DEBUG OFF");
	if (all || G.opt.term_log)
		ol_line(&n, G.opt.term_log ? "OPTION TERM LOG ON" : "OPTION TERM LOG OFF");
	if (all || (G.opt.wifi_country[0] &&
		    !(G.opt.wifi_country[0] == 'U' && G.opt.wifi_country[1] == 'S' &&
		      G.opt.wifi_country[2] == 0)))
	{
		if (n)
			mmb_out("\n");
		mmb_out("OPTION WIFI COUNTRY \"");
		mmb_out(mmb_opt_wifi_country());
		mmb_out("\"");
		n++;
	}
	if (all || G.opt.search_path[0])
	{
		if (n)
			mmb_out("\n");
		mmb_out("OPTION SEARCH PATH \"");
		mmb_out(G.opt.search_path);
		mmb_out("\"");
		n++;
	}
	for (i = 0; i < 12; i++)
	{
		if (G.opt.fkey[i][0])
		{
			if (n)
				mmb_out("\n");
			mmb_out("OPTION F");
			mmb_outf(0, i + 1);
			mmb_out(" \"");
			mmb_out(G.opt.fkey[i]);
			mmb_out("\"");
			n++;
		}
	}
	if (n == 0)
		mmb_out(all ? "No options" : "No options set");
}
