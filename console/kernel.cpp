#include "kernel.h"
#include "mmbasic.h"
#include <circle/new.h>
#include <circle/util.h>
#include <circle/timer.h>
#include <circle/usb/usbhid.h>

extern void mmb_platform_bind(CKernel *k);

static const char FromKernel[] = "console";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
#ifdef MMB_CIRCLE_WLAN
	m_Serial (&m_Interrupt),
#endif
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_Storage (&m_Interrupt, &m_Timer, &m_ActLED),
	m_pKeyboard (0),
	m_pKbdBuf (0),
	m_nBreak (0),
	m_nCad (0),
	m_nLen (0),
	m_nPos (0),
	m_nEsc (0),
	m_nCsiArg (0),
	m_nHist (0),
	m_nHistIdx (-1)
{
	m_Line[0] = '\0';
	m_Draft[0] = '\0';
	memset (m_Hist, 0, sizeof m_Hist);
	m_RepeatSeq[0] = '\0';
	m_RepeatLen = 0;
	m_HoldMs = 0;
	m_LastRepeatMs = 0;
	m_DidRepeat = 0;
	m_HeldHid = 0;
	m_LastMods = 0;
	m_AltHidSent = 0;
	m_NavHidSent = 0;
	m_UsbBurst = 0;
	memset (m_RawKeys, 0, sizeof m_RawKeys);
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

	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Screen.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (115200);
	if (bOK) bOK = m_Timer.Initialize ();
	if (bOK) bOK = m_Logger.Initialize (&m_Null);
	if (bOK)
		m_Storage.Initialize ();
	if (bOK)
		mmb_platform_bind (this);

	return bOK;
}

static void emit_n (CKernel *k, const void *p, unsigned n)
{
	if (!k || !p || !n)
		return;
	if (mmb_opt_console_serial ())
		k->Serial ().Write (p, n);
	if (mmb_opt_console_screen ())
		k->Screen ().Write (p, n);
}

static void emit (CKernel *k, const char *s)
{
	if (s)
		emit_n (k, s, (unsigned) strlen (s));
}

static void emit_prompt (CKernel *k)
{
	emit (k, mmb_prompt ());
}

static void emit_nl_prompt (CKernel *k)
{
	emit (k, "\r\n");
	emit_prompt (k);
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

static unsigned now_ms (void)
{
	return CTimer::GetClockTicks () / 1000;
}

/* CMM2 KEYDOWN codes used by the compat games. */
static int hid_to_cmm2 (unsigned char hid)
{
	if (hid >= 0x04 && hid <= 0x1d)
		return (int)('a' + (hid - 0x04));
	if (hid >= 0x1e && hid <= 0x26)
		return (int)('1' + (hid - 0x1e));
	if (hid == 0x27)
		return (int)'0';
	if (hid == 0x28)
		return 10; /* Enter */
	if (hid == 0x29)
		return 27; /* Esc */
	if (hid == 0x2c)
		return 32; /* Space */
	if (hid == 0x2d)
		return (int)'-';
	if (hid == 0x2e)
		return (int)'=';
	if (hid == 0x2f)
		return (int)'[';
	if (hid == 0x30)
		return (int)']';
	if (hid == 0x31)
		return (int)'\\';
	if (hid == 0x33)
		return (int)';';
	if (hid == 0x34)
		return (int)'\'';
	if (hid == 0x36)
		return (int)',';
	if (hid == 0x37)
		return (int)'.';
	if (hid == 0x38)
		return 47; /* / */
	if (hid == 0x4f)
		return 131; /* Right */
	if (hid == 0x50)
		return 130; /* Left */
	if (hid == 0x51)
		return 129; /* Down */
	if (hid == 0x52)
		return 128; /* Up */
	if (hid == 0x22)
		return 53; /* 5 */
	return 0;
}

/* Letters and digits for TUI Alt+key. Circle's cooked keymap returns
 * KeyNone for Left Alt, so USB Alt never reaches ProcessChar otherwise. */
static int hid_alt_char (unsigned char hid)
{
	int c = hid_to_cmm2 (hid);
	if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return c;
	return 0;
}

void CKernel::ApplyRawKeys (void)
{
	int codes[6];
	int n = 0, i;
	for (i = 0; i < 6 && n < 6; i++)
	{
		int c;
		if (m_RawKeys[i] == 0)
			continue;
		c = hid_to_cmm2 (m_RawKeys[i]);
		if (c)
			codes[n++] = c;
	}
	mmb_keydown_set (codes, n);
}

void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers,
				   const unsigned char RawKeys[6], void *pArg)
{
	CKernel *pThis = (CKernel *) pArg;
	unsigned i;
	unsigned char held = 0;
	int have_del = 0;
	if (pThis == 0)
		return;
	pThis->m_LastMods = ucModifiers;
	for (i = 0; i < 6; i++)
	{
		pThis->m_RawKeys[i] = RawKeys[i];
		/* USB HID: 0x46 Print Screen, 0x48 Pause/Break */
		if (RawKeys[i] == 0x46 || RawKeys[i] == 0x48)
			pThis->m_nBreak = 1;
		/* 0x4C Keyboard Delete Forward */
		if (RawKeys[i] == 0x4C)
			have_del = 1;
		if (!held && RawKeys[i] >= 4)
			held = RawKeys[i];
	}
	if (have_del &&
	    (ucModifiers & (LCTRL | RCTRL)) != 0 &&
	    (ucModifiers & (ALT | ALTGR)) != 0)
		pThis->m_nCad = 1;
	if (held != pThis->m_HeldHid)
	{
		pThis->m_HeldHid = held;
		pThis->m_HoldMs = now_ms ();
		pThis->m_DidRepeat = 0;
		pThis->m_LastRepeatMs = pThis->m_HoldMs;
		if (!held)
			pThis->m_RepeatLen = 0;
	}
	pThis->ApplyRawKeys ();
}

int CKernel::AltHeld (void) const
{
	return (m_LastMods & ALT) != 0;
}

void CKernel::PollUsbAlt (void)
{
	unsigned char hid;
	int ch;

	if ((m_LastMods & ALT) == 0 || (m_LastMods & (LCTRL | RCTRL)) != 0)
	{
		m_AltHidSent = 0;
		return;
	}
	if (!mmb_in_editor () && !mmb_in_files () && !mmb_in_wordpad ())
		return;
	hid = m_HeldHid;
	if (hid == 0 || hid == m_AltHidSent)
		return;
	ch = hid_alt_char (hid);
	if (ch == 0)
	{
		m_AltHidSent = hid;
		return;
	}
	m_AltHidSent = hid;
	m_UsbBurst = 1;
	ProcessChar (1, m_Line, &m_nLen);
	ProcessChar ((char) ch, m_Line, &m_nLen);
	m_UsbBurst = 0;
}

/*
 * Circle's cooked keymap maps Shift+arrows/Ins/Del to KeyNone, so those
 * chords never reach the editor. Inject xterm CSI with a modifier byte
 * (2=Shift, 5=Ctrl, 6=Ctrl+Shift) while the TUI editor is active.
 */
void CKernel::PollUsbEditorNav (void)
{
	unsigned char hid, mods;
	int shift, ctrl;
	char seq[8];
	unsigned n = 0, i;
	int tilde = 0;
	char letter = 0;
	int mod;

	if (!mmb_in_editor () && !mmb_in_wordpad ())
	{
		m_NavHidSent = 0;
		return;
	}
	if ((m_LastMods & ALT) != 0)
		return;

	hid = m_HeldHid;
	if (hid == 0)
	{
		m_NavHidSent = 0;
		return;
	}
	if (hid == m_NavHidSent)
		return;

	mods = m_LastMods;
	shift = (mods & (LSHIFT | RSHIFT)) != 0;
	ctrl = (mods & (LCTRL | RCTRL)) != 0;

	switch (hid)
	{
	case 0x52: letter = 'A'; break;
	case 0x51: letter = 'B'; break;
	case 0x4F: letter = 'C'; break;
	case 0x50: letter = 'D'; break;
	case 0x4A: tilde = 1; break;
	case 0x4B: tilde = 5; break;
	case 0x4C: tilde = 3; break;
	case 0x4D: tilde = 4; break;
	case 0x4E: tilde = 6; break;
	case 0x49: tilde = 2; break;
	default:
		return;
	}

	if (!shift && !(ctrl && hid == 0x49))
		return;
	if (ctrl && !shift && hid != 0x49)
		return;

	mod = 1 + (shift ? 1 : 0) + (ctrl ? 4 : 0);
	seq[n++] = 0x1b;
	seq[n++] = '[';
	if (letter)
	{
		seq[n++] = '1';
		seq[n++] = ';';
		seq[n++] = (char) ('0' + mod);
		seq[n++] = letter;
	}
	else
	{
		seq[n++] = (char) ('0' + tilde);
		seq[n++] = ';';
		seq[n++] = (char) ('0' + mod);
		seq[n++] = '~';
	}

	m_NavHidSent = hid;
	if (n < sizeof (m_RepeatSeq))
	{
		memcpy (m_RepeatSeq, seq, n);
		m_RepeatLen = n;
	}
	m_UsbBurst = 1;
	for (i = 0; i < n; i++)
		ProcessChar (seq[i], m_Line, &m_nLen);
	m_UsbBurst = 0;
}

void CKernel::PollUsbRepeat (void)
{
	unsigned now, first, next;
	unsigned i;
	if (!m_HeldHid || m_RepeatLen == 0)
		return;
	if (m_RepeatLen >= 1 && (unsigned char) m_RepeatSeq[0] == 1)
		return;
	if (m_RepeatLen >= 2 && (unsigned char) m_RepeatSeq[0] == 0x1b &&
	    m_RepeatSeq[1] != '[' && m_RepeatSeq[1] != 'O')
		return;
	now = now_ms ();
	first = (unsigned) mmb_opt_repeat_first ();
	next = (unsigned) mmb_opt_repeat_next ();
	if (first == 0)
		first = 600;
	if (next == 0)
		next = 150;
	if (!m_DidRepeat)
	{
		if (now - m_HoldMs < first)
			return;
		m_DidRepeat = 1;
		m_LastRepeatMs = now;
	}
	else if (now - m_LastRepeatMs < next)
		return;
	else
		m_LastRepeatMs = now;
	m_UsbBurst = 1;
	for (i = 0; i < m_RepeatLen; i++)
		ProcessChar (m_RepeatSeq[i], m_Line, &m_nLen);
	m_UsbBurst = 0;
}

void CKernel::PollInputChars (int breakKey)
{
	char tmp[32];
	int nBytes, i;

	AttachKeyboard ();
	ApplyRawKeys ();
	PollCadReboot ();
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
		if (c == '\r' || c == '\n')
			mmb_inkey_push (13);
		else if (c >= 32 || c == 8 || c == 9 || c == 27)
			mmb_inkey_push ((int) c);
	}
}

int CKernel::TakeBreak (void)
{
	int v = m_nBreak;
	m_nBreak = 0;
	return v;
}

void CKernel::PollCadReboot (void)
{
	if (!m_nCad)
		return;
	m_nCad = 0;
	mmb_reboot ();
}

void CKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	(void) pDevice;
	delete pThis->m_pKbdBuf;
	pThis->m_pKbdBuf = 0;
	pThis->m_pKeyboard = 0;
	pThis->m_HeldHid = 0;
	pThis->m_LastMods = 0;
	pThis->m_AltHidSent = 0;
	pThis->m_NavHidSent = 0;
}

void CKernel::LineGoEnd (char *Line, unsigned *pLen)
{
	while (m_nPos < *pLen)
	{
		emit_n (this, &Line[m_nPos], 1);
		m_nPos++;
	}
}

void CKernel::LineClearVis (char *Line, unsigned *pLen)
{
	LineGoEnd (Line, pLen);
	while (*pLen > 0)
	{
		(*pLen)--;
		emit (this, "\b \b");
	}
	m_nPos = 0;
	Line[0] = '\0';
}

void CKernel::LineReplace (char *Line, unsigned *pLen, const char *s)
{
	unsigned i;
	LineClearVis (Line, pLen);
	if (!s)
		s = "";
	for (i = 0; s[i] && *pLen < sizeof (m_Line) - 1; i++)
	{
		Line[(*pLen)++] = s[i];
		emit_n (this, &s[i], 1);
	}
	Line[*pLen] = '\0';
	m_nPos = *pLen;
}

void CKernel::LineLeft (void)
{
	if (m_nPos == 0)
		return;
	m_nPos--;
	emit (this, "\b");
}

void CKernel::LineRight (char *Line, unsigned *pLen)
{
	if (m_nPos >= *pLen)
		return;
	emit_n (this, &Line[m_nPos], 1);
	m_nPos++;
}

void CKernel::LineHome (void)
{
	while (m_nPos > 0)
	{
		m_nPos--;
		emit (this, "\b");
	}
}

void CKernel::LineInsert (char c, char *Line, unsigned *pLen)
{
	unsigned i, rest;
	if (*pLen >= sizeof (m_Line) - 1)
		return;
	if (m_nPos > *pLen)
		m_nPos = *pLen;
	rest = *pLen - m_nPos;
	if (rest)
		memmove (Line + m_nPos + 1, Line + m_nPos, rest);
	Line[m_nPos] = c;
	(*pLen)++;
	Line[*pLen] = '\0';
	for (i = m_nPos; i < *pLen; i++)
		emit_n (this, &Line[i], 1);
	for (i = 0; i < rest; i++)
		emit (this, "\b");
	m_nPos++;
}

void CKernel::LineBackspace (char *Line, unsigned *pLen)
{
	unsigned i, rest;
	if (m_nPos == 0)
		return;
	m_nPos--;
	rest = *pLen - m_nPos - 1;
	if (rest)
		memmove (Line + m_nPos, Line + m_nPos + 1, rest);
	(*pLen)--;
	Line[*pLen] = '\0';
	emit (this, "\b");
	for (i = 0; i < rest; i++)
		emit_n (this, &Line[m_nPos + i], 1);
	emit (this, " ");
	for (i = 0; i < rest + 1; i++)
		emit (this, "\b");
}

void CKernel::LineDelete (char *Line, unsigned *pLen)
{
	unsigned i, rest;
	if (m_nPos >= *pLen)
		return;
	rest = *pLen - m_nPos - 1;
	if (rest)
		memmove (Line + m_nPos, Line + m_nPos + 1, rest);
	(*pLen)--;
	Line[*pLen] = '\0';
	for (i = 0; i < rest; i++)
		emit_n (this, &Line[m_nPos + i], 1);
	emit (this, " ");
	for (i = 0; i < rest + 1; i++)
		emit (this, "\b");
}

void CKernel::HistAdd (const char *s)
{
	if (!s || !s[0])
		return;
	if (m_nHist > 0 && strcmp (m_Hist[m_nHist - 1], s) == 0)
		return;
	if (m_nHist >= (unsigned) HistMax)
	{
		memmove (m_Hist[0], m_Hist[1],
			 (HistMax - 1) * sizeof m_Hist[0]);
		m_nHist = (unsigned) HistMax - 1;
	}
	strncpy (m_Hist[m_nHist], s, sizeof m_Hist[0] - 1);
	m_Hist[m_nHist][sizeof m_Hist[0] - 1] = '\0';
	m_nHist++;
	m_nHistIdx = -1;
}

void CKernel::HistUp (char *Line, unsigned *pLen)
{
	if (m_nHist == 0)
		return;
	if (m_nHistIdx < 0)
	{
		unsigned n = *pLen;
		if (n >= sizeof m_Draft)
			n = sizeof m_Draft - 1;
		memcpy (m_Draft, Line, n);
		m_Draft[n] = '\0';
		m_nHistIdx = (int) m_nHist - 1;
	}
	else if (m_nHistIdx > 0)
		m_nHistIdx--;
	LineReplace (Line, pLen, m_Hist[m_nHistIdx]);
}

void CKernel::HistDown (char *Line, unsigned *pLen)
{
	if (m_nHistIdx < 0)
		return;
	if (m_nHistIdx >= (int) m_nHist - 1)
	{
		m_nHistIdx = -1;
		LineReplace (Line, pLen, m_Draft);
		return;
	}
	m_nHistIdx++;
	LineReplace (Line, pLen, m_Hist[m_nHistIdx]);
}

void CKernel::HandleCsi (char final, char *Line, unsigned *pLen)
{
	if (final == 'A')
		HistUp (Line, pLen);
	else if (final == 'B')
		HistDown (Line, pLen);
	else if (final == 'C')
		LineRight (Line, pLen);
	else if (final == 'D')
		LineLeft ();
	else if (final == 'H')
		LineHome ();
	else if (final == 'F')
		LineGoEnd (Line, pLen);
	else if (final == '~')
	{
		if (m_nCsiArg == 3)
			LineDelete (Line, pLen);
		else if (m_nCsiArg == 1 || m_nCsiArg == 7)
			LineHome ();
		else if (m_nCsiArg == 4 || m_nCsiArg == 8)
			LineGoEnd (Line, pLen);
	}
}

void CKernel::ProcessChar (char c, char *Line, unsigned *pLen)
{
	if (mmb_in_editor ())
	{
		const char *out = mmb_editor_key (c);
		emit (this, out);
		if (!mmb_in_editor ())
		{
			if (mmb_in_files ())
			{
				emit (this, mmb_files_on_editor_exit ());
				if (!mmb_in_files ())
					emit_prompt (this);
			}
			else
				emit_prompt (this);
		}
		return;
	}

	if (mmb_in_files ())
	{
		const char *out = mmb_files_key (c);
		emit (this, out);
		if (!mmb_in_files () && !mmb_in_editor ())
			emit_prompt (this);
		return;
	}

	if (mmb_in_ihelp ())
	{
		const char *out = mmb_ihelp_key (c);
		emit (this, out);
		if (!mmb_in_ihelp ())
			emit_prompt (this);
		return;
	}

	if (mmb_in_term ())
	{
		const char *out = mmb_term_key (c);
		if (out && out[0])
			emit (this, out);
		if (!mmb_in_term ())
			emit_prompt (this);
		return;
	}

	if (mmb_in_wordpad ())
	{
		const char *out = mmb_wordpad_key (c);
		if (out && out[0])
			emit (this, out);
		if (!mmb_in_wordpad ())
			emit_prompt (this);
		return;
	}

	if (mmb_in_connect ())
	{
		const char *out = mmb_connect_key (c);
		if (out && out[0])
			emit (this, out);
		if (!mmb_in_connect ())
			emit_prompt (this);
		return;
	}

	/* ESC / CSI from Circle keymap (arrows, Home/End/Delete, F-keys). */
	if (m_nEsc == 1)
	{
		if (c == '[')
		{
			m_nEsc = 2;
			m_nCsiArg = 0;
			return;
		}
		if (c == 'O')
		{
			m_nEsc = 3;
			return;
		}
		m_nEsc = 0;
		if (c == 0x1b)
		{
			m_nEsc = 1;
			return;
		}
		/* ESC+letter is Alt (serial stand-in / USB Meta). Ignore at prompt. */
		return;
	}
	if (m_nEsc == 2)
	{
		if (c >= '0' && c <= '9')
		{
			m_nCsiArg = m_nCsiArg * 10 + (c - '0');
			return;
		}
		if (c == ';')
		{
			m_nCsiArg = 0;
			return;
		}
		m_nEsc = 0;
		HandleCsi (c, Line, pLen);
		return;
	}
	if (m_nEsc == 3)
	{
		m_nEsc = 0;
		HandleCsi (c, Line, pLen);
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
		emit_n (this, &echo, 1);

		Line[*pLen] = '\0';
		HistAdd (Line);
		m_nPos = 0;
		m_nHistIdx = -1;
		const char *Result = mmb_exec_line (Line);
		if (mmb_in_editor ())
		{
			/* Editor streams a full frame via write_screen/write_serial. */
			if (Result && Result[0])
				emit (this, Result);
		}
		else if (mmb_take_home_prompt ())
		{
			emit (this, Result);
			emit_prompt (this);
		}
		else if (mmb_in_files ())
		{
			/* Dual-pane TUI already streamed to HDMI/serial. */
		}
		else if (mmb_in_ihelp ())
		{
			/* Interactive HELP TUI already streamed to HDMI/serial. */
		}
		else if (mmb_in_term ())
		{
			if (Result && Result[0])
				emit (this, Result);
		}
		else if (mmb_in_wordpad ())
		{
			if (Result && Result[0])
				emit (this, Result);
		}
		else if (mmb_in_connect ())
		{
			if (Result && Result[0])
				emit (this, Result);
		}
		else if (!Result || !Result[0])
			emit_nl_prompt (this);
		else
		{
			emit (this, "\r\n");
			emit (this, Result);
			emit_nl_prompt (this);
		}
		*pLen = 0;
		m_nPos = 0;
	}
	else if (c == 8 || c == 127)
		LineBackspace (Line, pLen);
	else if (c == 3)
	{
		*pLen = 0;
		m_nPos = 0;
		m_nHistIdx = -1;
		emit_nl_prompt (this);
	}
	else if (c == '\t' || (unsigned char) c < 32)
	{
		/* Tab and other controls must not enter Line[]. */
	}
	else
		LineInsert (c, Line, pLen);
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
		PollCadReboot ();
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
				emit_n (this, "\r\n", 2);
				return 0;
			}
			if (c == 8 || c == 127)
			{
				if (n > 0)
				{
					n--;
					emit_n (this, "\b \b", 3);
				}
				continue;
			}
			if (n + 1 < maxn)
			{
				char e = hide ? '*' : c;
				buf[n++] = c;
				emit_n (this, &e, 1);
			}
		}
	}
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "console ready");

	const char Banner[] = "MMBASIC-CONSOLE READY\r\n";
	emit_n (this, Banner, sizeof (Banner) - 1);
	emit_prompt (this);

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
			{
				if ((unsigned) nKbd < sizeof (m_RepeatSeq))
				{
					memcpy (m_RepeatSeq, Buffer + nBytes, (unsigned) nKbd);
					m_RepeatLen = (unsigned) nKbd;
				}
				nBytes += nKbd;
			}
		}

		PollUsbAlt ();
		PollUsbEditorNav ();
		PollCadReboot ();
		if (nBytes <= 0)
		{
			PollUsbRepeat ();
			continue;
		}

		m_UsbBurst = 1;
		for (int i = 0; i < nBytes; i++)
			ProcessChar (Buffer[i], m_Line, &m_nLen);
		m_UsbBurst = 0;
	}

	return ShutdownHalt;
}
