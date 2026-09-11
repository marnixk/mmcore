/* Host-side stand-in for circle/net/netbuffer.h (tests only): a heap
   buffer with the head/private-data operations the TCP receiver uses. */
#ifndef _circle_net_netbuffer_h
#define _circle_net_netbuffer_h

#include <assert.h>
#include <string.h>
#include <circle/types.h>

#define FRAME_BUFFER_SIZE 1600

class CNetBuffer
{
public:
	enum TPurpose { Receive, TCPSend };

	CNetBuffer (TPurpose Purpose, size_t ulLength = 0, const void *pBuffer = nullptr)
	:	m_pNext (nullptr), m_ulLength (ulLength), m_ulPrivateDataLength (0)
	{
		(void) Purpose;
		assert (ulLength <= FRAME_BUFFER_SIZE);
		m_pHead = m_Buffer;
		if (pBuffer && ulLength)
			memcpy (m_pHead, pBuffer, ulLength);
	}

	void *GetPtr (void) const { return m_pHead; }
	size_t GetLength (void) const { return m_ulLength; }

	void *RemoveHeader (size_t ulLength)
	{
		assert (ulLength);
		assert (m_ulLength >= ulLength);
		m_pHead += ulLength;
		m_ulLength -= ulLength;
		return m_pHead;
	}

	void RemoveTrailer (size_t ulLength)
	{
		assert (m_ulLength >= ulLength);
		m_ulLength -= ulLength;
	}

	void SetPrivateData (const void *pBuffer, size_t ulLength)
	{
		assert (ulLength <= sizeof m_PrivateData);
		memcpy (m_PrivateData, pBuffer, ulLength);
		m_ulPrivateDataLength = ulLength;
	}
	const void *GetPrivateData (void) const { return m_PrivateData; }
	size_t GetPrivateDataLength (void) const { return m_ulPrivateDataLength; }

	CNetBuffer *m_pNext;

private:
	u8 m_Buffer[FRAME_BUFFER_SIZE];
	u8 *m_pHead;
	size_t m_ulLength;
	u8 m_PrivateData[20];
	size_t m_ulPrivateDataLength;
};

#endif
