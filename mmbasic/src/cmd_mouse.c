/*
 * MOUSE command parsing and per-console reset (#792).
 *
 * The cursor sprite, non-dirtying overlay and event classification live in
 * mouse_cursor.c; this file is the thin BASIC surface around them:
 *
 *   MOUSE ON | OFF
 *   MOUSE CURSOR [pointer|hand|crosshair|questionmark|deny|0..4]
 *
 * ON MOUSECLICK / ON MOUSEMOVE handler names are parsed by mmb_cmd_on() in
 * cmd_lang.c and stored in the per-console interpreter state.
 */
#include "mmb_priv.h"

void mmb_cmd_mouse(void)
{
	mmb_skip_sp();
	if (mmb_match("OFF"))
	{
		mmb_mouse_cursor_set_on(0);
		return;
	}
	if (mmb_match("ON"))
	{
		mmb_mouse_cursor_set_on(1);
		return;
	}
	if (mmb_match("CURSOR"))
	{
		int type = 0; /* no argument resets to the pointer */
		mmb_skip_sp();
		if (*G.p && *G.p != ':' && *G.p != '\'' && *G.p != 0)
		{
			if (mmb_is_ident_start(*G.p))
			{
				char id[MMB_MAX_NAME];
				mmb_ident(id, (int)sizeof(id));
				type = mmb_mouse_cursor_type_from_name(id);
				if (type < 0)
					mmb_syntax();
			}
			else if (*G.p == '"')
			{
				mmb_val v = mmb_expr();
				if (v.type != T_STR)
					mmb_syntax();
				type = mmb_mouse_cursor_type_from_name(
					v.s ? v.s : "");
				if (type < 0)
					mmb_syntax();
			}
			else
			{
				mmb_val v = mmb_expr();
				type = (int)mmb_as_int(v);
				if (type < 0 || type > 4)
					mmb_syntax();
			}
		}
		mmb_mouse_cursor_set_type(type);
		return;
	}
	mmb_syntax();
}

void mmb_mouse_reset_all(void)
{
	mmb_mouse_cursor_reset_all();
}
