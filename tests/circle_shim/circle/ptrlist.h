/* Host-side stand-in for circle/ptrlist.h (tests only). Same interface
   and nullptr conventions as Circle's CPtrList. */
#ifndef _circle_ptrlist_h
#define _circle_ptrlist_h

#include <assert.h>

struct TPtrListElement
{
	void *pPtr;
	TPtrListElement *pPrev;
	TPtrListElement *pNext;
};

class CPtrList
{
public:
	CPtrList (void) : m_pFirst (nullptr) {}
	~CPtrList (void)
	{
		while (m_pFirst)
			Remove (m_pFirst);
	}

	TPtrListElement *GetFirst (void) const { return m_pFirst; }
	TPtrListElement *GetNext (TPtrListElement *pElement) const { return pElement->pNext; }
	static void *GetPtr (TPtrListElement *pElement) { return pElement->pPtr; }

	void InsertBefore (TPtrListElement *pAfter, void *pPtr)
	{
		assert (pAfter);
		TPtrListElement *e = new TPtrListElement;
		e->pPtr = pPtr;
		e->pNext = pAfter;
		e->pPrev = pAfter->pPrev;
		if (pAfter->pPrev)
			pAfter->pPrev->pNext = e;
		else
			m_pFirst = e;
		pAfter->pPrev = e;
	}

	void InsertAfter (TPtrListElement *pBefore, void *pPtr)
	{
		TPtrListElement *e = new TPtrListElement;
		e->pPtr = pPtr;
		if (!pBefore)
		{
			assert (!m_pFirst);
			e->pPrev = nullptr;
			e->pNext = nullptr;
			m_pFirst = e;
			return;
		}
		e->pPrev = pBefore;
		e->pNext = pBefore->pNext;
		if (pBefore->pNext)
			pBefore->pNext->pPrev = e;
		pBefore->pNext = e;
	}

	void Remove (TPtrListElement *pElement)
	{
		if (pElement->pPrev)
			pElement->pPrev->pNext = pElement->pNext;
		else
			m_pFirst = pElement->pNext;
		if (pElement->pNext)
			pElement->pNext->pPrev = pElement->pPrev;
		delete pElement;
	}

private:
	TPtrListElement *m_pFirst;
};

#endif
