#include "mmb_priv.h"

#define TK_ESC ((char)0x80)
#define TK_LEN 3

static const char *const kws[] = {
	"AND_PIXELS", "ATN2", "ATN", "ATAN", "ACOS", "ACS", "ASIN", "ASN",
	"AND", "AS", "ABS", "ARC", "APPEND", "AUDIO",
	"BITMAP", "BLIT", "BOX", "BIN$", "BREAK", "BACKUP", "BACKWARD",
	"BEGIN", "BOTH", "BARE", "BASE", "BAUDRATE",
	"CONTINUE", "CONST", "CONNECT", "COPY", "CLOSE", "CLS", "CIRCLE",
	"CREDITS", "CAT", "CASE", "CALL", "CHDIR", "CHR$", "COS", "COSH",
	"COLOR", "COLOUR", "COLOURS", "COLORS", "COLOURCODE", "COLORCODE",
	"CONSOLE", "CRLF", "CR", "CLEAR", "CHI_P", "CHI", "CORREL", "CWD$",
	"CPUSPEED", "CLOCK", "CREATE",
	"DIM", "DO", "DIR", "DRIVE", "DEC", "DEL", "DATA", "DATE$", "DEFAULT",
	"DEGREES", "DOWN", "DISPLAY", "DS3231", "DISABLE", "DOTPRODUCT",
	"DOT", "DRAW", "DEVICE",
	"END", "ENDIF", "ELSEIF", "ELSE", "EXIT", "ERROR", "EDIT", "ERASE",
	"EXP", "EXPLICIT", "ENABLE", "ESCAPE", "EOF",
	"FOR", "FUNCTION", "FILES", "FONT", "FRAMEBUFFER",
	"FACTORY_RESET", "FACTORY", "FLOAT", "FIX", "FORWARD", "FILL",
	"FLASH", "FAST",
	"GOTO", "GOSUB", "GUI",
	"HELP", "HEX$", "HEADING", "HDMI", "HDMI0", "HEARTBEAT",
	"IHELP", "IF", "INC", "INPUT", "INPUT$", "INT", "INTEGER", "IMAGE",
	"INSTR", "IPCONFIG", "INKEY$", "IS",
	"KILL",
	"LOCAL", "LOOP", "LIST", "LS", "LINE", "LEN", "LEFT$",
	"LCASE$", "LOAD", "LOG", "LOG10", "LOF", "LOC", "LEGACY", "LARGE",
	"LOWER", "LCDPANEL", "LEFT", "LENGTH",
	"MEMORY", "MID$", "MKDIR", "MODE", "MATH", "MOD", "MV", "MOUSE",
	"MILLISECONDS", "MEDIUM", "MONITOR", "MAX", "MIN", "MEAN", "MEDIAN",
	"MAGNITUDE", "MM.HRES", "MM.VRES", "MM.INFO$", "MM.INFO", "MM.VER",
	"MM.DEVICE$", "MM.CMDLINE$",
	"NEXT", "NEW", "NOT", "NAME", "NONE", "NOLED", "NORMAL",
	"OPEN", "OPTION", "OPTIONS", "OR", "OR_PIXELS", "ON", "OFF", "OUTPUT",
	"OCT$",
	"PRINT", "PIXEL", "PAGE", "PLAY", "PAUSE", "PACKAGE",
	"POLYGON", "PLAYING", "PI", "PIN", "PROMPT", "PROFILING", "PNG",
	"PWM", "PHASE", "PEN",
	"RETURN", "RUN", "READ", "RESTORE", "RANDOMIZE", "RBOX", "RMDIR",
	"RM", "RENAME", "REBOOT", "RESTART", "RESET", "RGB", "RND", "RIGHT$",
	"RADIANS", "REVERSE", "RAM", "RTC", "REPEAT", "RESOLUTION",
	"RESIZE_FAST", "RESIZE", "ROTATE_FAST", "ROTATE", "RIGHT",
	"SUB", "STRUCT", "STATIC", "SELECT", "SAVE", "SEEK", "SORT", "SPRITE",
	"SETTICK", "STRING", "STRING$", "STR$", "SQR", "SQRT", "SIN", "SINH",
	"SGN", "SPACE$", "STEP", "SCREEN", "SERIAL", "STATUS", "SWAP",
	"SLEEP", "SD", "SEARCH", "SMALL", "SUM", "SCALE", "SET",
	"TO", "THEN", "TERM", "TRIANGLE", "TEXT", "TURTLE", "TIMER", "TIME$",
	"TAN", "TANH", "TAB", "TITLE", "TOUCH", "TURN", "TV", "TIMING", "TYPE",
	"UNTIL", "UCASE$", "UP", "UPPER", "USBKEYBOARD",
	"VAL", "VSYNC_WAIT", "WHILE", "WEND", "WORDPAD",
	"WARP_H", "WARP_V", "WINDOW", "WRITE", "WIFI", "VERY", "XOR",
	"XOR_PIXELS", "Y_AXIS",
	"FORMAT$", "CHOICE", "BOUND", "CINT", "EVAL", "KEYDOWN", "ASC",
	"DEG", "RAD", "POS", "EXTRACT", "INSERT",
	0
};

static const char *kwu[512];
static int nkw;
static char *tprog;
static int tcap;
static char tline[MMB_LINE_LEN];
static int tok_ready;

static void init_kw(void);

static int ident_eq(const char *a, const char *b)
{
	while (*a && *b)
	{
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

static int lookup_kw(const char *name)
{
	int i;
	for (i = 0; i < nkw; i++)
		if (ident_eq(name, kwu[i]))
			return i + 1;
	return 0;
}

int mmb_kw_id(const char *kw)
{
	init_kw();
	return lookup_kw(kw);
}

static void emit_tok(char *dst, int *o, int id)
{
	dst[(*o)++] = TK_ESC;
	dst[(*o)++] = (char)(id & 0xff);
	dst[(*o)++] = (char)((id >> 8) & 0xff);
}

static int tok_id_at(const char *p)
{
	if ((unsigned char)*p != (unsigned char)TK_ESC)
		return 0;
	return (unsigned char)p[1] | ((unsigned char)p[2] << 8);
}

static void init_kw(void)
{
	int i;
	if (nkw)
		return;
	for (i = 0; kws[i]; i++)
	{
		if (lookup_kw(kws[i]))
			continue;
		if (nkw >= 511)
			break;
		kwu[nkw++] = kws[i];
	}
}

void mmb_tokenize_text(const char *src, char *dst, int dstsz)
{
	int o = 0;
	init_kw();
	while (*src && o < dstsz - 4)
	{
		if (*src == '"')
		{
			dst[o++] = *src++;
			while (*src && o < dstsz - 2)
			{
				dst[o++] = *src;
				if (*src == '"')
				{
					src++;
					if (*src == '"')
					{
						if (o < dstsz - 2)
							dst[o++] = *src++;
						else
							break;
						continue;
					}
					break;
				}
				src++;
			}
			continue;
		}
		if (*src == '\'')
		{
			while (*src && o < dstsz - 1)
				dst[o++] = *src++;
			break;
		}
		if ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') || *src == '_')
		{
			char name[MMB_MAX_NAME];
			int n = 0, id;
			while (mmb_is_ident(*src) && n < MMB_MAX_NAME - 2)
			{
				char c = *src++;
				if (c >= 'a' && c <= 'z')
					c = (char)(c - 32);
				name[n++] = c;
			}
			if (*src == '$' || *src == '%' || *src == '!')
				name[n++] = *src++;
			name[n] = 0;
			id = lookup_kw(name);
			if (id && o < dstsz - 4)
				emit_tok(dst, &o, id);
			else
			{
				int i;
				for (i = 0; i < n && o < dstsz - 1; i++)
					dst[o++] = name[i];
			}
			continue;
		}
		if (*src == '?')
		{
			int id = lookup_kw("PRINT");
			if (id && o < dstsz - 4)
			{
				emit_tok(dst, &o, id);
				src++;
				continue;
			}
		}
		dst[o++] = *src++;
	}
	dst[o] = 0;
}

void mmb_tokenize_program(void)
{
	int i, cap;
	init_kw();
	if (G.nprog <= 0)
	{
		tok_ready = 0;
		return;
	}
	if (G.nprog > tcap)
	{
		cap = G.nprog + 32;
		if (cap > MMB_MAX_LINES)
			cap = MMB_MAX_LINES;
		if (!G.plat || !G.plat->alloc)
		{
			tok_ready = 0;
			return;
		}
		tprog = G.plat->alloc((unsigned)cap * MMB_LINE_LEN);
		tcap = tprog ? cap : 0;
	}
	if (!tprog)
	{
		tok_ready = 0;
		return;
	}
	for (i = 0; i < G.nprog; i++)
		mmb_tokenize_text(G.prog[i], tprog + i * MMB_LINE_LEN, MMB_LINE_LEN);
	tok_ready = 1;
}

const char *mmb_tok_line(int pc)
{
	if (!tok_ready || !tprog || pc < 0 || pc >= G.nprog)
		return G.prog[pc];
	return tprog + pc * MMB_LINE_LEN;
}

const char *mmb_tok_immediate(const char *src)
{
	mmb_tokenize_text(src, tline, MMB_LINE_LEN);
	return tline;
}

int mmb_tok_expand(char *dst, int dstsz)
{
	int id, n;
	const char *s;
	init_kw();
	id = tok_id_at(G.p);
	if (id < 1 || id > nkw)
		return 0;
	s = kwu[id - 1];
	n = 0;
	while (*s && n < dstsz - 1)
		dst[n++] = *s++;
	dst[n] = 0;
	G.p += TK_LEN;
	return 1;
}

int mmb_match_token(const char *kw)
{
	int id, got;
	if ((unsigned char)*G.p != (unsigned char)TK_ESC)
		return 0;
	id = mmb_kw_id(kw);
	got = tok_id_at(G.p);
	if (!id || got != id)
		return 0;
	G.p += TK_LEN;
	return 1;
}
