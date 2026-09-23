#include "kernel.h"
#include "mmbasic.h"
#include "frontend.h"
#include "session.h"
#include <circle/alloc.h>
#include <circle/font.h>
#include <circle/new.h>
#include <circle/util.h>
#include <circle/timer.h>
#include <circle/usb/usbhid.h>

extern void mmb_platform_bind(CKernel *k);

extern "C" const unsigned char mmb_cp437_8x16[];

static const TFont FontCP437 =
{
	8,
	16,
	0,
	0x00,
	0xFF,
	mmb_cp437_8x16
};

static const char FromKernel[] = "console";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight (), FontCP437),
#ifdef MMB_CIRCLE_NET
	m_Serial (&m_Interrupt),
#endif
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_Storage (&m_Interrupt, &m_Timer, &m_ActLED),
	m_pKeyboard (0),
	m_pKbdBuf (0),
	m_pMouse (0),
	m_MousePresent (0),
	m_MouseX (0),
	m_MouseY (0),
	m_MouseButtons (0),
	m_MouseWheel (0),
	m_nBreak (0),
	m_nCad (0)
{
	m_RepeatSeq[0] = '\0';
	m_RepeatLen = 0;
	m_HoldMs = 0;
	m_LastRepeatMs = 0;
	m_DidRepeat = 0;
	m_HeldHid = 0;
	m_LastMods = 0;
	m_AltHidSent = 0;
	m_NavHidSent = 0;
	m_CharHidSent = 0;
	m_FkeyHidSent = 0;
	m_ConsoleHidSent = 0;
	m_ShotHidSent = 0;
	m_UsbBurst = 0;
	memset (m_RawKeys, 0, sizeof m_RawKeys);
	m_ActLED.Blink (2);
}

CKernel::~CKernel (void)
{
	delete m_pKbdBuf;
	m_pKbdBuf = 0;
	m_pKeyboard = 0;
	/* CUSBMouseDevice owns the CMouseDevice; just drop our pointer. */
	m_pMouse = 0;
	m_MousePresent = 0;
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Screen.Initialize ();
	if (bOK)
		m_Screen.SetCursorBlock (TRUE);
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
	const unsigned char *c = (const unsigned char *) p;
	unsigned left = n;
	if (!k || !p || !n)
		return;
	if (mmb_opt_console_serial ())
	{
		/* Circle's buffered serial Write drops the tail when its TX ring
		 * fills, so retry until everything has been queued (bounded so a
		 * stalled reader cannot hang the kernel forever). */
		unsigned spins = 0;
		while (left && spins < 100000)
		{
			int w = k->Serial ().Write (c, left);
			if (w <= 0)
			{
				CTimer::SimpleusDelay (200);
				spins++;
				continue;
			}
			c += (unsigned) w;
			left -= (unsigned) w;
		}
	}
	if (mmb_opt_console_screen ())
		k->Screen ().Write (p, n);
}

/* Output sink for the portable front end (mmb_front_*). */
static void front_emit (void *ctx, const char *s, unsigned n)
{
	emit_n ((CKernel *) ctx, s, n);
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

/* Attach the first USB HID mouse as a raw pointer source (no Circle cursor).
 * The status handler runs in USB interrupt context and only updates the
 * accumulated position/buttons; the paint app reads them at task level. */
void CKernel::AttachMouse (void)
{
	if (m_pMouse != 0)
		return;
	m_pMouse = (CMouseDevice *) m_DeviceNameService.GetDevice ("mouse1", FALSE);
	if (m_pMouse == 0)
		return;
	m_pMouse->RegisterStatusHandler (MouseStatusHandler, this);
	m_MouseX = (int) (m_Screen.GetWidth () / 2);
	m_MouseY = (int) (m_Screen.GetHeight () / 2);
	m_MousePresent = 1;
}

void CKernel::MouseStatusHandler (unsigned nButtons, int nDisplacementX,
				  int nDisplacementY, int nWheelMove, void *pArg)
{
	CKernel *pThis = (CKernel *) pArg;
	int x, y;

	if (pThis == 0)
		return;
	x = pThis->m_MouseX + nDisplacementX;
	y = pThis->m_MouseY + nDisplacementY;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= (int) pThis->m_Screen.GetWidth ())
		x = (int) pThis->m_Screen.GetWidth () - 1;
	if (y >= (int) pThis->m_Screen.GetHeight ())
		y = (int) pThis->m_Screen.GetHeight () - 1;
	pThis->m_MouseX = x;
	pThis->m_MouseY = y;
	pThis->m_MouseButtons = (int) nButtons;
	pThis->m_MouseWheel += nWheelMove;
}

void CKernel::MouseState (int *present, int *x, int *y, int *buttons, int *wheel)
{
	if (present != 0)
		*present = m_pMouse != 0 && m_MousePresent;
	if (x != 0)
		*x = m_MouseX;
	if (y != 0)
		*y = m_MouseY;
	if (buttons != 0)
		*buttons = m_MouseButtons;
	if (wheel != 0)
		*wheel = m_MouseWheel;
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
		{
			pThis->m_RepeatLen = 0;
			/* Release arrived between polls: allow the same key again. */
			pThis->m_NavHidSent = 0;
			pThis->m_CharHidSent = 0;
		}
	}
	pThis->ApplyRawKeys ();
}

int CKernel::AltHeld (void) const
{
	return (m_LastMods & ALT) != 0;
}

int CKernel::CtrlAltHeld (void) const
{
	return (m_LastMods & ALT) != 0 && (m_LastMods & (LCTRL | RCTRL)) != 0;
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
	if (!mmb_in_editor () && !mmb_in_files () && !mmb_in_wordpad () &&
	    !mmb_in_term () && !mmb_in_connect ())
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
	ProcessChar (1);
	ProcessChar ((char) ch);
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
	{
		int alt_left, alt_right;
		if ((m_LastMods & (LCTRL | RCTRL)) != 0)
		{
			/* Ctrl+Alt is the character-picker chord: arrows navigate
			 * while the picker is open, other keys stay cooked. */
			PollUsbCharNav ();
			return;
		}
		hid = m_HeldHid;
		if (hid == 0)
		{
			m_NavHidSent = 0;
			return;
		}
		if (hid == m_NavHidSent)
			return;
		alt_left = (hid == 0x50);
		alt_right = (hid == 0x4F);
		if (!alt_left && !alt_right)
			return;
		seq[0] = 0x1b;
		seq[1] = '[';
		seq[2] = '1';
		seq[3] = ';';
		seq[4] = '3';
		seq[5] = alt_left ? 'D' : 'C';
		n = 6;
		m_NavHidSent = hid;
		m_UsbBurst = 1;
		for (i = 0; i < n; i++)
			ProcessChar (seq[i]);
		m_UsbBurst = 0;
		return;
	}

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

	/* Ctrl+Enter is a cooked Return that the keymap cannot modify.
	 * Inject CSI 29~ as the Replace All chord. */
	if (ctrl && (hid == 0x28 || hid == 0x58) && mmb_in_editor ())
	{
		seq[0] = 0x1b;
		seq[1] = '[';
		seq[2] = '2';
		seq[3] = '9';
		seq[4] = '~';
		n = 5;
		m_NavHidSent = hid;
		m_UsbBurst = 1;
		for (i = 0; i < n; i++)
			ProcessChar (seq[i]);
		m_UsbBurst = 0;
		return;
	}

	/* Circle maps Shift+Tab to Tab. Inject CSI Z (backtab) so the
	 * editor outdents instead of indenting. */
	if (hid == 0x2B && shift && mmb_in_editor ())
	{
		seq[0] = 0x1b;
		seq[1] = '[';
		seq[2] = 'Z';
		n = 3;
		m_NavHidSent = hid;
		if (n < sizeof (m_RepeatSeq))
		{
			memcpy (m_RepeatSeq, seq, n);
			m_RepeatLen = n;
		}
		m_UsbBurst = 1;
		for (i = 0; i < n; i++)
			ProcessChar (seq[i]);
		m_UsbBurst = 0;
		return;
	}

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
		ProcessChar (seq[i]);
	m_UsbBurst = 0;
}

/*
 * While the editor's Ctrl+Alt character picker is open, Circle's cooked keymap
 * yields nothing for Ctrl+Alt+arrows, so inject plain cursor sequences and let
 * the editor move the picker selection. Circle yields nothing cooked for
 * Ctrl+Alt+Enter either, so inject a Return to insert the selection.
 */
void CKernel::PollUsbCharNav (void)
{
	unsigned char hid;
	char seq[4];
	unsigned n = 0, i;
	char final = 0;
	int enter = 0;

	if (!mmb_in_editor () || !mmb_editor_char_picker_active ())
	{
		m_CharHidSent = 0;
		return;
	}
	hid = m_HeldHid;
	if (hid == 0)
	{
		m_CharHidSent = 0;
		return;
	}
	if (hid == m_CharHidSent)
		return;
	switch (hid)
	{
	case 0x52: final = 'A'; break; /* Up */
	case 0x51: final = 'B'; break; /* Down */
	case 0x4F: final = 'C'; break; /* Right */
	case 0x50: final = 'D'; break; /* Left */
	case 0x4A: final = 'H'; break; /* Home */
	case 0x4D: final = 'F'; break; /* End */
	case 0x28: /* Return */
	case 0x58: /* Keypad Enter */
		enter = 1;
		break;
	default:
		return;
	}
	if (enter)
		seq[n++] = '\r';
	else
	{
		seq[n++] = 0x1b;
		seq[n++] = '[';
		seq[n++] = final;
	}
	m_CharHidSent = hid;
	m_UsbBurst = 1;
	for (i = 0; i < n; i++)
		ProcessChar (seq[i]);
	m_UsbBurst = 0;
}

void CKernel::PollUsbFKeys (void)
{
	unsigned char hid;
	char seq[8];
	unsigned n = 0, i;

	if (!mmb_in_term () && !mmb_in_connect ())
	{
		m_FkeyHidSent = 0;
		return;
	}
	if ((m_LastMods & ALT) != 0)
		return;
	hid = m_HeldHid;
	if (hid == 0)
	{
		m_FkeyHidSent = 0;
		return;
	}
	if (hid == m_FkeyHidSent)
		return;
	if (hid != 0x43)
		return;
	seq[n++] = 0x1b;
	seq[n++] = '[';
	seq[n++] = '2';
	seq[n++] = '1';
	seq[n++] = '~';
	m_FkeyHidSent = hid;
	m_UsbBurst = 1;
	for (i = 0; i < n; i++)
		ProcessChar (seq[i]);
	m_UsbBurst = 0;
}

/*
 * Ctrl+Alt+F1..F4 switch virtual consoles, Linux-style. Circle's cooked
 * keymap yields nothing usable for the chord, so read the raw HID state and
 * translate the function-key codes ourselves. No-op unless Ctrl and Alt are
 * both held.
 */
void CKernel::PollUsbConsole (void)
{
	unsigned char hid;
	int idx = -1;

	if ((m_LastMods & ALT) == 0 || (m_LastMods & (LCTRL | RCTRL)) == 0)
	{
		m_ConsoleHidSent = 0;
		return;
	}
	hid = m_HeldHid;
	if (hid == 0 || hid == m_ConsoleHidSent)
		return;
	switch (hid)
	{
	case 0x3A: idx = 0; break; /* F1 */
	case 0x3B: idx = 1; break; /* F2 */
	case 0x3C: idx = 2; break; /* F3 */
	case 0x3D: idx = 3; break; /* F4 */
	default:
		return;
	}
	m_ConsoleHidSent = hid;
	mmb_console_switch(idx);
}

/*
 * F12 captures a screenshot from anywhere. Circle's cooked keymap yields
 * nothing for F10..F12, so inject the front end's F12 sequence (ESC [ 2 4 ~)
 * from the raw HID state. Plain F12 only; Alt/Ctrl chords are left alone.
 */
void CKernel::PollUsbScreenshot (void)
{
	unsigned char hid;
	char seq[8];
	unsigned n = 0, i;

	if ((m_LastMods & ALT) != 0)
	{
		m_ShotHidSent = 0;
		return;
	}
	hid = m_HeldHid;
	if (hid == 0 || hid == m_ShotHidSent)
	{
		if (hid == 0)
			m_ShotHidSent = 0;
		return;
	}
	if (hid != 0x45) /* F12 */
		return;
	seq[n++] = 0x1b;
	seq[n++] = '[';
	seq[n++] = '2';
	seq[n++] = '4';
	seq[n++] = '~';
	m_ShotHidSent = hid;
	m_UsbBurst = 1;
	for (i = 0; i < n; i++)
		ProcessChar (seq[i]);
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
		first = MMB_REPEAT_FIRST_DEFAULT;
	if (next == 0)
		next = MMB_REPEAT_NEXT_DEFAULT;
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
		ProcessChar (m_RepeatSeq[i]);
	m_UsbBurst = 0;
}

void CKernel::PollInputChars (int breakKey)
{
	char tmp[32];
	int nBytes, i;

	AttachKeyboard ();
	AttachMouse ();
	ApplyRawKeys ();
	PollUsbConsole ();
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
	pThis->m_CharHidSent = 0;
	pThis->m_FkeyHidSent = 0;
	pThis->m_ShotHidSent = 0;
}

/* The interactive line editor, history and ESC/CSI decoding live in
   mmbasic/src/frontend.c (mmb_front_*); this backend only turns raw
   keyboard bytes into the feed and filters Circle's cooked-key duplicates. */

void CKernel::ProcessChar (char c)
{
	/* Circle's cooked keyboard map turns USB Shift+Tab into Tab and
	 * Ctrl+Enter into Return; PollUsbEditorNav already injected the real
	 * sequences, so drop the duplicates the editor would otherwise see. */
	if (mmb_in_editor ())
	{
		if (c == '\t' && (m_LastMods & (LSHIFT | RSHIFT)) != 0)
			return;
		if ((c == '\n' || c == '\r') && (m_LastMods & (LCTRL | RCTRL)) != 0 &&
		    !mmb_editor_char_picker_active ())
			return;
	}
	mmb_front_feed_byte (c);
}

int CKernel::ReadLine (char **out, int hide)
{
	unsigned cap = 128, n = 0;
	char *buf;
	if (out == 0)
		return -1;
	*out = 0;
	buf = (char *) malloc (cap);
	if (buf == 0)
		return -1;
	buf[0] = 0;
	for (;;)
	{
		char tmp[8];
		int nBytes, i;
		mmb_poll ();
		AttachKeyboard ();
		AttachMouse ();
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
		{
			free (buf);
			return -2;
		}
		if (nBytes <= 0)
			continue;
		for (i = 0; i < nBytes; i++)
		{
			char c = tmp[i];
			int bk = mmb_break_key ();
			if ((bk && (unsigned char) c == (unsigned char) bk) || TakeBreak ())
			{
				free (buf);
				return -2;
			}
			if (c == '\r' || c == '\n')
			{
				buf[n] = 0;
				emit_n (this, "\r\n", 2);
				*out = buf;
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
			if (n + 2 >= cap)
			{
				unsigned ncap = cap * 2;
				char *nb = (char *) realloc (buf, ncap);
				if (nb == 0)
				{
					free (buf);
					return -1;
				}
				buf = nb;
				cap = ncap;
			}
			{
				char e = hide ? '*' : c;
				buf[n++] = c;
				emit_n (this, &e, 1);
			}
		}
	}
}

int CKernel::ReadRaw (unsigned char *buf, unsigned n)
{
	unsigned got = 0;
	unsigned start = CTimer::GetClockTicks ();

	if (n == 0)
		return 0;
	if (!buf)
		return -1;
	while (got < n)
	{
		char tmp[4096];
		unsigned want = n - got;
		int nBytes;

		if (want > sizeof tmp)
			want = sizeof tmp;
		nBytes = m_Serial.Read (tmp, want);
		if (nBytes > 0)
		{
			memcpy (buf + got, tmp, (size_t) nBytes);
			got += (unsigned) nBytes;
			start = CTimer::GetClockTicks ();
		}
		else
		{
			mmb_poll ();
			/* A raw transfer that goes this long without a byte is dead:
			 * the sender lost data to a receive overrun. Fail fast so
			 * the uploader can resync and retry (XFER over serial). */
			if ((CTimer::GetClockTicks () - start) / 1000u > 5000u)
				return -1;
		}
	}
	return 0;
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "console ready");

	mmb_front_init (front_emit, this);
	mmb_console_init ();
	mmb_print_startup ();
	mmb_front_prompt ();

	AttachKeyboard ();
	AttachMouse ();

	for (;;)
	{
		mmb_poll ();
		mmb_console_poll ();
		AttachKeyboard ();
		AttachMouse ();

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
		PollUsbFKeys ();
		PollUsbConsole ();
		PollUsbScreenshot ();
		PollCadReboot ();
		if (nBytes <= 0)
		{
			PollUsbRepeat ();
			continue;
		}

		m_UsbBurst = 1;
		for (int i = 0; i < nBytes; i++)
			ProcessChar (Buffer[i]);
		m_UsbBurst = 0;
	}

	return ShutdownHalt;
}
