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
#include <circle/input/mouse.h>
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
	int ReadLine (char **out, int hide);
	int ReadRaw (unsigned char *buf, unsigned n);
	void PollInputChars (int breakKey);
	int TakeBreak (void);
	int AltHeld (void) const;
	int CtrlAltHeld (void) const;
	/* Latest USB mouse state; *present is 0 when no mouse is attached. */
	void MouseState (int *present, int *x, int *y, int *buttons, int *wheel);

private:
	void AttachKeyboard (void);
	void AttachMouse (void);
	void ProcessChar (char c);
	void PollUsbRepeat (void);
	void PollUsbAlt (void);
	void PollUsbEditorNav (void);
	void PollUsbCharNav (void);
	void PollUsbFKeys (void);
	void PollUsbConsole (void);
	void PollUsbScreenshot (void);
	void PollUsbAppPicker (void);
	void PollCadReboot (void);
	void ApplyRawKeys (void);

	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);
	static void KeyStatusHandlerRaw (unsigned char ucModifiers,
					 const unsigned char RawKeys[6], void *pArg);
	static void MouseStatusHandler (unsigned nButtons, int nDisplacementX,
					int nDisplacementY, int nWheelMove,
					void *pArg);

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
	CMouseDevice		* volatile m_pMouse;
	volatile int		m_MousePresent;
	volatile int		m_MouseX;
	volatile int		m_MouseY;
	volatile int		m_MouseButtons;
	volatile int		m_MouseWheel;
	volatile int		m_nBreak;
	volatile int		m_nCad;
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
	unsigned char		m_CharHidSent;
	unsigned char		m_FkeyHidSent;
	unsigned char		m_ConsoleHidSent;
	unsigned char		m_ShotHidSent;
	unsigned char		m_PickerHidSent;
	int			m_UsbBurst;
};

#endif
