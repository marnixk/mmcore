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

// Parse a non-negative integer, advancing *pp past the digits.
static boolean ParseInt (const char **pp, int *pValue)
{
	const char *p = SkipSpaces (*pp);
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
	*pValue = v;
	*pp = p;
	return TRUE;
}

// Minimal placeholder interpreter so the QEMU harness has deterministic,
// correctness-checkable behaviour to test before the real MMBasic core
// (from PicoMite-fork) is ported onto Circle. Supports:
//   PRINT "text"        -> text
//   PRINT <a> [+-*/ <b>] -> integer result
// Anything else         -> ?SYNTAX ERROR
static void Evaluate (const char *pLine, CString *pOut)
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

			// echo the keystroke to both serial and screen
			m_Serial.Write (&c, 1);
			m_Screen.Write (&c, 1);

			if (c == '\r' || c == '\n')
			{
				Line[nLen] = '\0';

				CString Result;
				Evaluate (Line, &Result);

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
