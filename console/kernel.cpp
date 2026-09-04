#include "kernel.h"
#include <circle/string.h>
#include <circle/util.h>

static const char FromKernel[] = "console";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer)
{
	m_ActLED.Blink (2);
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK) bOK = m_Screen.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (115200);
	if (bOK) bOK = m_Logger.Initialize (&m_Screen);
	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Timer.Initialize ();

	return bOK;
}

static const char *SkipSpaces (const char *p)
{
	while (*p == ' ' || *p == '\t')
	{
		p++;
	}
	return p;
}

static boolean ParseInt (const char **pp, int *pValue)
{
	const char *p = SkipSpaces (*pp);
	boolean neg = FALSE;
	if (*p == '-')
	{
		neg = TRUE;
		p++;
	}
	if (*p < '0' || *p > '9')
	{
		return FALSE;
	}
	int v = 0;
	while (*p >= '0' && *p <= '9')
	{
		v = v * 10 + (*p - '0');
		p++;
	}
	*pValue = neg ? -v : v;
	*pp = p;
	return TRUE;
}

// Split a comma-separated argument list into trimmed string fields.
static int SplitFields (const char *p, CString *pFields, int nMax)
{
	int n = 0;
	while (*p != '\0' && n < nMax)
	{
		p = SkipSpaces (p);
		CString Field;
		while (*p != '\0' && *p != ',')
		{
			Field.Append (*p);
			p++;
		}
		Field.TrimRight ();
		pFields[n++] = Field;
		if (*p == ',')
		{
			p++;
		}
	}
	return n;
}

static TScreenColor ParseColor (const CString &rName)
{
	const char *p = (const char *) rName;
	if      (strcmp (p, "BLACK")   == 0) return BLACK_COLOR;
	else if (strcmp (p, "RED")     == 0) return BRIGHT_RED_COLOR;
	else if (strcmp (p, "GREEN")   == 0) return BRIGHT_GREEN_COLOR;
	else if (strcmp (p, "BLUE")    == 0) return BRIGHT_BLUE_COLOR;
	else if (strcmp (p, "YELLOW")  == 0) return BRIGHT_YELLOW_COLOR;
	else if (strcmp (p, "CYAN")    == 0) return BRIGHT_CYAN_COLOR;
	else if (strcmp (p, "MAGENTA") == 0) return BRIGHT_MAGENTA_COLOR;
	return BRIGHT_WHITE_COLOR;
}

static boolean FieldIsInt (const CString &rField, int *pValue)
{
	const char *p = (const char *) rField;
	return ParseInt (&p, pValue) && *SkipSpaces (p) == '\0';
}

static void Plot (CScreenDevice *pScreen, int x, int y, TScreenColor c)
{
	if (x >= 0 && y >= 0
	    && (unsigned) x < pScreen->GetWidth ()
	    && (unsigned) y < pScreen->GetHeight ())
	{
		pScreen->SetPixel (x, y, c);
	}
}

static void DrawLine (CScreenDevice *pScreen, int x0, int y0, int x1, int y1,
		      TScreenColor c)
{
	int dx = x1 - x0; if (dx < 0) dx = -dx;
	int dy = y1 - y0; if (dy < 0) dy = -dy;
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;
	for (;;)
	{
		Plot (pScreen, x0, y0, c);
		if (x0 == x1 && y0 == y1)
		{
			break;
		}
		int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 <  dx) { err += dx; y0 += sy; }
	}
}

static void DrawBox (CScreenDevice *pScreen, int x, int y, int w, int h,
		     TScreenColor c)
{
	DrawLine (pScreen, x,         y,         x + w - 1, y,         c);
	DrawLine (pScreen, x,         y + h - 1, x + w - 1, y + h - 1, c);
	DrawLine (pScreen, x,         y,         x,         y + h - 1, c);
	DrawLine (pScreen, x + w - 1, y,         x + w - 1, y + h - 1, c);
}

static void DrawCircle (CScreenDevice *pScreen, int cx, int cy, int r,
			TScreenColor c)
{
	int x = r, y = 0, err = 0;
	while (x >= y)
	{
		Plot (pScreen, cx + x, cy + y, c);
		Plot (pScreen, cx + y, cy + x, c);
		Plot (pScreen, cx - y, cy + x, c);
		Plot (pScreen, cx - x, cy + y, c);
		Plot (pScreen, cx - x, cy - y, c);
		Plot (pScreen, cx - y, cy - x, c);
		Plot (pScreen, cx + y, cy - x, c);
		Plot (pScreen, cx + x, cy - y, c);
		y++;
		if (err <= 0) { err += 2 * y + 1; }
		if (err >  0) { x--; err -= 2 * x + 1; }
	}
}

static void ClearScreen (CScreenDevice *pScreen, TScreenColor c)
{
	unsigned w = pScreen->GetWidth ();
	unsigned h = pScreen->GetHeight ();
	for (unsigned y = 0; y < h; y++)
	{
		for (unsigned x = 0; x < w; x++)
		{
			pScreen->SetPixel (x, y, c);
		}
	}
}

// Minimal placeholder interpreter so the QEMU harness has deterministic,
// correctness-checkable behaviour before the real MMBasic core (from
// PicoMite-fork) is ported onto Circle. Text:
//   PRINT "text"          -> text
//   PRINT <a> [+-*/ <b>]  -> integer result
// CMM2-style graphics (drawn on the shared framebuffer, no text response):
//   CLS [colour]
//   PIXEL  x,y[,colour]
//   LINE   x1,y1,x2,y2[,colour]
//   BOX    x,y,w,h[,colour]
//   CIRCLE x,y,r[,colour]
// Anything else            -> ?SYNTAX ERROR
static void Evaluate (CScreenDevice *pScreen, const char *pLine, CString *pOut)
{
	const char *p = SkipSpaces (pLine);

	if (strncmp (p, "PRINT", 5) == 0 && (p[5] == ' ' || p[5] == '\0'))
	{
		p = SkipSpaces (p + 5);

		if (*p == '"')
		{
			p++;
			CString Text;
			while (*p != '\0' && *p != '"')
			{
				Text.Append (*p);
				p++;
			}
			*pOut = Text;
			return;
		}

		int a;
		if (ParseInt (&p, &a))
		{
			p = SkipSpaces (p);
			char op = *p;
			if (op == '+' || op == '-' || op == '*' || op == '/')
			{
				p++;
				int b;
				if (ParseInt (&p, &b))
				{
					int r = 0;
					switch (op)
					{
					case '+': r = a + b; break;
					case '-': r = a - b; break;
					case '*': r = a * b; break;
					case '/': r = (b != 0) ? a / b : 0; break;
					}
					pOut->Format ("%d", r);
					return;
				}
			}
			else if (op == '\0')
			{
				pOut->Format ("%d", a);
				return;
			}
		}

		*pOut = "?SYNTAX ERROR";
		return;
	}

	// graphics commands: <KEYWORD> <arg,arg,...>
	struct { const char *kw; int nNum; } GfxCmds[] = {
		{ "CLS",    0 }, { "PIXEL",  2 }, { "LINE",   4 },
		{ "BOX",    4 }, { "CIRCLE", 3 },
	};

	for (unsigned i = 0; i < sizeof (GfxCmds) / sizeof (GfxCmds[0]); i++)
	{
		size_t len = strlen (GfxCmds[i].kw);
		if (strncmp (p, GfxCmds[i].kw, len) == 0
		    && (p[len] == ' ' || p[len] == '\0'))
		{
			CString Fields[6];
			int nFields = SplitFields (SkipSpaces (p + len), Fields, 6);

			// optional trailing colour field (may be present even when 0
			// numeric args are required, e.g. CLS RED)
			TScreenColor col = BRIGHT_WHITE_COLOR;
			int nArgs = nFields;
			int probe;
			if (nFields > 0 && !FieldIsInt (Fields[nFields - 1], &probe))
			{
				col = ParseColor (Fields[nFields - 1]);
				nArgs = nFields - 1;
			}

			if (nArgs != GfxCmds[i].nNum)
			{
				*pOut = "?SYNTAX ERROR";
				return;
			}

			int v[4] = { 0, 0, 0, 0 };
			for (int k = 0; k < nArgs; k++)
			{
				if (!FieldIsInt (Fields[k], &v[k]))
				{
					*pOut = "?SYNTAX ERROR";
					return;
				}
			}

			if      (strcmp (GfxCmds[i].kw, "CLS")    == 0)
				ClearScreen (pScreen, nFields > 0 ? col : BLACK_COLOR);
			else if (strcmp (GfxCmds[i].kw, "PIXEL")  == 0)
				Plot (pScreen, v[0], v[1], col);
			else if (strcmp (GfxCmds[i].kw, "LINE")   == 0)
				DrawLine (pScreen, v[0], v[1], v[2], v[3], col);
			else if (strcmp (GfxCmds[i].kw, "BOX")    == 0)
				DrawBox (pScreen, v[0], v[1], v[2], v[3], col);
			else if (strcmp (GfxCmds[i].kw, "CIRCLE") == 0)
				DrawCircle (pScreen, v[0], v[1], v[2], col);

			*pOut = "";
			return;
		}
	}

	*pOut = "?SYNTAX ERROR";
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "console ready");

	const char Banner[] = "MMBASIC-CONSOLE READY\r\n> ";
	m_Serial.Write (Banner, sizeof (Banner) - 1);
	m_Screen.Write (Banner, sizeof (Banner) - 1);

	char Line[128];
	unsigned nLen = 0;

	for (;;)
	{
		char Buffer[64];
		int nBytes = m_Serial.Read (Buffer, sizeof (Buffer));
		if (nBytes <= 0)
		{
			continue;
		}

		for (int i = 0; i < nBytes; i++)
		{
			char c = Buffer[i];

			m_Serial.Write (&c, 1);
			m_Screen.Write (&c, 1);

			if (c == '\r' || c == '\n')
			{
				Line[nLen] = '\0';

				CString Result;
				Evaluate (&m_Screen, Line, &Result);

				CString Out;
				Out.Format ("\r\n%s\r\n> ", (const char *) Result);
				m_Serial.Write ((const char *) Out, strlen ((const char *) Out));
				m_Screen.Write ((const char *) Out, strlen ((const char *) Out));

				nLen = 0;
			}
			else if (nLen < sizeof (Line) - 1)
			{
				Line[nLen++] = c;
			}
		}
	}

	return ShutdownHalt;
}
