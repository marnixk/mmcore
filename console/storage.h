#ifndef CONSOLE_STORAGE_H
#define CONSOLE_STORAGE_H

#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/actled.h>
#include <circle/usb/usbhcidevice.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/types.h>

class CStorage
{
public:
	CStorage (CInterruptSystem *irq, CTimer *timer, CActLED *led);
	~CStorage (void);

	boolean Initialize (void);
	void Poll (void);

	FATFS		m_fs[FF_VOLUMES];

private:
	CEMMCDevice	m_EMMC;
	CUSBHCIDevice	m_USBHCI;
};

void mmb_storage_bind (CStorage *st);

#endif
