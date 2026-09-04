#include "kernel.h"
#include "mmbasic.h"
#include <circle/util.h>

extern void mmb_platform_bind(CKernel *k);

static const char FromKernel[] = "console";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_Storage (&m_Interrupt, &m_Timer, &m_ActLED)
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
	if (bOK) bOK = m_Logger.Initialize (&m_Null);
	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Timer.Initialize ();
	if (bOK)
		m_Storage.Initialize ();
	if (bOK)
		mmb_platform_bind (this);

	return bOK;
}

static void emit (CSerialDevice *ser, CScreenDevice *scr, const char *s)
{
	unsigned n = (unsigned) strlen (s);
	if (n)
	{
		ser->Write (s, n);
		scr->Write (s, n);
	}
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "console ready");

	const char Banner[] = "MMBASIC-CONSOLE READY\r\n> ";
	m_Serial.Write (Banner, sizeof (Banner) - 1);
	m_Screen.Write (Banner, sizeof (Banner) - 1);

	char Line[256];
	unsigned nLen = 0;

	for (;;)
	{
		mmb_poll ();
		char Buffer[64];
		int nBytes = m_Serial.Read (Buffer, sizeof (Buffer));
		if (nBytes <= 0)
			continue;

		for (int i = 0; i < nBytes; i++)
		{
			char c = Buffer[i];

			if (mmb_in_editor ())
			{
				const char *out = mmb_editor_key (c);
				emit (&m_Serial, &m_Screen, out);
				if (!mmb_in_editor ())
					emit (&m_Serial, &m_Screen, "> ");
				continue;
			}

			m_Serial.Write (&c, 1);
			m_Screen.Write (&c, 1);

			if (c == '\r' || c == '\n')
			{
				Line[nLen] = '\0';
				const char *Result = mmb_exec_line (Line);
				if (mmb_in_editor ())
				{
					emit (&m_Serial, &m_Screen, "\r\n");
					emit (&m_Serial, &m_Screen, Result);
				}
				else
				{
					emit (&m_Serial, &m_Screen, "\r\n");
					emit (&m_Serial, &m_Screen, Result);
					emit (&m_Serial, &m_Screen, "\r\n> ");
				}
				nLen = 0;
			}
			else if (c == 8 || c == 127)
			{
				if (nLen > 0)
					nLen--;
			}
			else if (nLen < sizeof (Line) - 1)
			{
				Line[nLen++] = c;
			}
		}
	}

	return ShutdownHalt;
}
