#include "kernel.h"
#include "mmbasic.h"
#include <circle/new.h>
#include <circle/util.h>

extern void mmb_platform_bind(CKernel *k);

static const char FromKernel[] = "console";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_Storage (&m_Interrupt, &m_Timer, &m_ActLED),
	m_pKeyboard (0),
	m_pKbdBuf (0),
	m_nBreak (0),
	m_nLen (0),
	m_nEsc (0)
{
	m_Line[0] = '\0';
	m_ActLED.Blink (2);
}

CKernel::~CKernel (void)
{
	delete m_pKbdBuf;
	m_pKbdBuf = 0;
	m_pKeyboard = 0;
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

void CKernel::AttachKeyboard (void)
{
	if (m_pKeyboard != 0)
		return;

	m_pKeyboard = (CUSBKeyboardDevice *)
		m_DeviceNameService.GetDevice ("ukbd1", FALSE);
	if (m_pKeyboard == 0)
		return;

	m_pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler, this);
	m_pKbdBuf = new CKeyboardBuffer (m_pKeyboard);
	/* Mixed mode: cooked keys still fill the buffer; raw sees PrtScr (HID 0x46). */
	m_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw, TRUE, this);
}

void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers,
				   const unsigned char RawKeys[6], void *pArg)
{
	CKernel *pThis = (CKernel *) pArg;
	unsigned i;
	(void) ucModifiers;
	if (pThis == 0)
		return;
	for (i = 0; i < 6; i++)
	{
		/* USB HID: 0x46 Print Screen, 0x48 Pause/Break */
		if (RawKeys[i] == 0x46 || RawKeys[i] == 0x48)
			pThis->m_nBreak = 1;
	}
}

void CKernel::PollInputChars (int breakKey)
{
	char tmp[32];
	int nBytes, i;

	AttachKeyboard ();
	nBytes = m_Serial.Read (tmp, sizeof tmp);
	if (nBytes < 0)
		nBytes = 0;
	if (m_pKbdBuf != 0)
	{
		int nKbd = m_pKbdBuf->Read (tmp + nBytes,
					    sizeof tmp - (size_t) nBytes);
		if (nKbd > 0)
			nBytes += nKbd;
	}
	for (i = 0; i < nBytes; i++)
	{
		unsigned char c = (unsigned char) tmp[i];
		if (breakKey && c == (unsigned char) breakKey)
			m_nBreak = 1;
	}
}

int CKernel::TakeBreak (void)
{
	int v = m_nBreak;
	m_nBreak = 0;
	return v;
}

void CKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	(void) pDevice;
	delete pThis->m_pKbdBuf;
	pThis->m_pKbdBuf = 0;
	pThis->m_pKeyboard = 0;
}

void CKernel::ProcessChar (char c, char *Line, unsigned *pLen)
{
	if (mmb_in_editor ())
	{
		const char *out = mmb_editor_key (c);
		emit (&m_Serial, &m_Screen, out);
		if (!mmb_in_editor ())
		{
			if (mmb_in_files ())
			{
				emit (&m_Serial, &m_Screen, mmb_files_on_editor_exit ());
				if (!mmb_in_files ())
					emit (&m_Serial, &m_Screen, "> ");
			}
			else
				emit (&m_Serial, &m_Screen, "> ");
		}
		return;
	}

	if (mmb_in_files ())
	{
		const char *out = mmb_files_key (c);
		emit (&m_Serial, &m_Screen, out);
		if (!mmb_in_files () && !mmb_in_editor ())
			emit (&m_Serial, &m_Screen, "> ");
		return;
	}

	if (mmb_in_connect ())
	{
		const char *out = mmb_connect_key (c);
		if (out && out[0])
			emit (&m_Serial, &m_Screen, out);
		if (!mmb_in_connect ())
			emit (&m_Serial, &m_Screen, "> ");
		return;
	}

	/* ESC / CSI from Circle keymap (arrows, Home/End/Delete/Insert, F-keys). */
	if (m_nEsc == 1)
	{
		if (c == '[' || c == 'O')
		{
			m_nEsc = 2;
			return;
		}
		m_nEsc = 0;
		if (c == 0x1b)
		{
			m_nEsc = 1;
			return;
		}
		return;
	}
	if (m_nEsc >= 2)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~')
			m_nEsc = 0;
		return;
	}
	if (c == 0x1b)
	{
		m_nEsc = 1;
		return;
	}

	if (c == '\r' || c == '\n')
	{
		/* USB Enter is '\n'; serial is usually '\r'. Echo CR so HDMI wraps. */
		char echo = '\r';
		m_Serial.Write (&echo, 1);
		m_Screen.Write (&echo, 1);

		Line[*pLen] = '\0';
		const char *Result = mmb_exec_line (Line);
		if (mmb_in_editor ())
		{
			/* Editor streams a full frame via write_screen/write_serial. */
			if (Result && Result[0])
				emit (&m_Serial, &m_Screen, Result);
		}
		else if (mmb_take_home_prompt ())
		{
			emit (&m_Serial, &m_Screen, Result);
			emit (&m_Serial, &m_Screen, "> ");
		}
		else if (mmb_in_files ())
		{
			/* Dual-pane TUI already streamed to HDMI/serial. */
		}
		else if (mmb_in_connect ())
		{
			if (Result && Result[0])
				emit (&m_Serial, &m_Screen, Result);
		}
		else
		{
			emit (&m_Serial, &m_Screen, "\r\n");
			emit (&m_Serial, &m_Screen, Result);
			emit (&m_Serial, &m_Screen, "\r\n> ");
		}
		*pLen = 0;
	}
	else if (c == 8 || c == 127)
	{
		if (*pLen > 0)
		{
			(*pLen)--;
			emit (&m_Serial, &m_Screen, "\b \b");
		}
	}
	else if (c == 3)
	{
		*pLen = 0;
		emit (&m_Serial, &m_Screen, "\r\n> ");
	}
	else if (c == '\t' || (unsigned char) c < 32)
	{
		/* Tab and other controls must not enter Line[]. */
	}
	else if (*pLen < sizeof (m_Line) - 1)
	{
		Line[(*pLen)++] = c;
		m_Serial.Write (&c, 1);
		m_Screen.Write (&c, 1);
	}
}


int CKernel::ReadLine (char *buf, unsigned maxn, int hide)
{
	unsigned n = 0;
	if (!buf || maxn == 0)
		return -1;
	buf[0] = 0;
	for (;;)
	{
		char tmp[8];
		int nBytes, i;
		mmb_poll ();
		AttachKeyboard ();
		nBytes = m_Serial.Read (tmp, sizeof tmp);
		if (nBytes < 0)
			nBytes = 0;
		if (m_pKbdBuf != 0)
		{
			int nKbd = m_pKbdBuf->Read (tmp + nBytes,
						    sizeof tmp - (size_t) nBytes);
			if (nKbd > 0)
				nBytes += nKbd;
		}
		if (TakeBreak ())
			return -2;
		if (nBytes <= 0)
			continue;
		for (i = 0; i < nBytes; i++)
		{
			char c = tmp[i];
			int bk = mmb_break_key ();
			if ((bk && (unsigned char) c == (unsigned char) bk) || TakeBreak ())
				return -2;
			if (c == '\r' || c == '\n')
			{
				buf[n] = 0;
				m_Serial.Write ("\r\n", 2);
				m_Screen.Write ("\r\n", 2);
				return 0;
			}
			if (c == 8 || c == 127)
			{
				if (n > 0)
				{
					n--;
					m_Serial.Write ("\b \b", 3);
					m_Screen.Write ("\b \b", 3);
				}
				continue;
			}
			if (n + 1 < maxn)
			{
				char e = hide ? '*' : c;
				buf[n++] = c;
				m_Serial.Write (&e, 1);
				m_Screen.Write (&e, 1);
			}
		}
	}
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "console ready");

	const char Banner[] = "MMBASIC-CONSOLE READY\r\n> ";
	m_Serial.Write (Banner, sizeof (Banner) - 1);
	m_Screen.Write (Banner, sizeof (Banner) - 1);

	AttachKeyboard ();

	for (;;)
	{
		mmb_poll ();
		AttachKeyboard ();

		char Buffer[64];
		int nBytes = m_Serial.Read (Buffer, sizeof (Buffer));
		if (nBytes < 0)
			nBytes = 0;

		if (m_pKbdBuf != 0)
		{
			int nKbd = m_pKbdBuf->Read (Buffer + nBytes,
						    sizeof (Buffer) - (size_t) nBytes);
			if (nKbd > 0)
				nBytes += nKbd;
		}

		if (nBytes <= 0)
			continue;

		for (int i = 0; i < nBytes; i++)
			ProcessChar (Buffer[i], m_Line, &m_nLen);
	}

	return ShutdownHalt;
}
