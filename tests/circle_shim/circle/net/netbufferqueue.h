/* Host-side stand-in for circle/net/netbufferqueue.h (tests only). */
#ifndef _circle_net_netbufferqueue_h
#define _circle_net_netbufferqueue_h

#include <circle/net/netbuffer.h>
#include <circle/types.h>

class CNetBufferQueue
{
public:
	CNetBufferQueue (boolean bProtected = FALSE)
	:	m_pFirst (nullptr), m_pLast (nullptr), m_nEntries (0), m_ulBytes (0)
	{
		(void) bProtected;
	}
	~CNetBufferQueue (void) { Flush (); }

	boolean IsEmpty (void) const { return m_pFirst == nullptr; }
	unsigned GetNumEntries (void) const { return m_nEntries; }
	size_t GetBytesQueued (void) const { return m_ulBytes; }

	void Flush (void)
	{
		CNetBuffer *p;
		while ((p = Dequeue ()) != nullptr)
			delete p;
	}

	void Enqueue (CNetBuffer *pNetBuffer)
	{
		pNetBuffer->m_pNext = nullptr;
		if (m_pLast)
			m_pLast->m_pNext = pNetBuffer;
		else
			m_pFirst = pNetBuffer;
		m_pLast = pNetBuffer;
		m_nEntries++;
		m_ulBytes += pNetBuffer->GetLength ();
	}

	CNetBuffer *Dequeue (void)
	{
		CNetBuffer *p = m_pFirst;
		if (!p)
			return nullptr;
		m_pFirst = p->m_pNext;
		if (!m_pFirst)
			m_pLast = nullptr;
		m_nEntries--;
		m_ulBytes -= p->GetLength ();
		p->m_pNext = nullptr;
		return p;
	}

private:
	CNetBuffer *m_pFirst;
	CNetBuffer *m_pLast;
	unsigned m_nEntries;
	size_t m_ulBytes;
};

#endif
