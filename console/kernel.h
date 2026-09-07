#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/nulldevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/keyboardbuffer.h>
#include <circle/types.h>
#include "storage.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);
	TShutdownMode Run (void);

	CScreenDevice &Screen (void) { return m_Screen; }
	CSerialDevice &Serial (void) { return m_Serial; }
	int ReadLine (char *buf, unsigned maxn, int hide);
	void PollInputChars (int breakKey);
	int TakeBreak (void);
	int AltHeld (void) const;

private:
	void AttachKeyboard (void);
	void ProcessChar (char c, char *Line, unsigned *pLen);
	void PollUsbRepeat (void);
	void PollUsbAlt (void);
	void PollUsbEditorNav (void);
	void PollCadReboot (void);
	void ApplyRawKeys (void);
	void LineGoEnd (char *Line, unsigned *pLen);
	void LineClearVis (char *Line, unsigned *pLen);
	void LineReplace (char *Line, unsigned *pLen, const char *s);
	void LineLeft (void);
	void LineRight (char *Line, unsigned *pLen);
	void LineHome (void);
	void LineInsert (char c, char *Line, unsigned *pLen);
	void LineBackspace (char *Line, unsigned *pLen);
	void LineDelete (char *Line, unsigned *pLen);
	void HistAdd (const char *s);
	void HistUp (char *Line, unsigned *pLen);
	void HistDown (char *Line, unsigned *pLen);
	void HandleCsi (char final, char *Line, unsigned *pLen);

	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);
	static void KeyStatusHandlerRaw (unsigned char ucModifiers,
					 const unsigned char RawKeys[6], void *pArg);

private:
	// do not change this order (Interrupt must exist before Serial)
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CTimer			m_Timer;
	CNullDevice		m_Null;
	CLogger			m_Logger;
	CStorage		m_Storage;

	CUSBKeyboardDevice	* volatile m_pKeyboard;
	CKeyboardBuffer		*m_pKbdBuf;
	volatile int		m_nBreak;
	volatile int		m_nCad;
	char			m_Line[256];
	unsigned		m_nLen;
	unsigned		m_nPos;
	int			m_nEsc;
	int			m_nCsiArg;
	enum { HistMax = 32 };
	char			m_Hist[HistMax][256];
	unsigned		m_nHist;
	int			m_nHistIdx;
	char			m_Draft[256];
	char			m_RepeatSeq[16];
	unsigned		m_RepeatLen;
	unsigned		m_HoldMs;
	unsigned		m_LastRepeatMs;
	int			m_DidRepeat;
	unsigned char		m_HeldHid;
	unsigned char		m_RawKeys[6];
	unsigned char		m_LastMods;
	unsigned char		m_AltHidSent;
	unsigned char		m_NavHidSent;
	int			m_UsbBurst;
};

#endif
