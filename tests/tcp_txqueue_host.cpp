/* Host test for TxQueue Flush: 1-byte ACKs and partial ACKs. */
#include <circle/net/netbufferqueue.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                          \
	do                                                                   \
	{                                                                    \
		if (!(cond))                                                 \
		{                                                            \
			printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_fail++;                                            \
		}                                                            \
	} while (0)

static CNetBuffer *buf (const char *s)
{
	return new CNetBuffer (CNetBuffer::TCPSend, strlen (s), s);
}

int main (void)
{
	CNetBufferQueue q;
	q.Enqueue (buf ("i"));
	q.Enqueue (buf ("r"));
	q.Enqueue (buf ("e"));
	CHECK (q.GetNumEntries () == 3);
	q.Flush (1);
	CHECK (q.GetNumEntries () == 2);
	CHECK (q.GetBytesQueued () == 2);
	q.Flush (1);
	q.Flush (1);
	CHECK (q.IsEmpty ());

	CNetBufferQueue p;
	p.Enqueue (buf ("ABCDEFGH"));
	p.Flush (3);
	CHECK (p.GetNumEntries () == 1);
	CHECK (p.GetBytesQueued () == 5);
	CNetBuffer *left = p.Dequeue ();
	CHECK (left != nullptr);
	CHECK (left->GetLength () == 5);
	CHECK (memcmp (left->GetPtr (), "DEFGH", 5) == 0);
	delete left;

	if (g_fail)
	{
		printf ("%d checks failed\n", g_fail);
		return 1;
	}
	printf ("all checks passed\n");
	return 0;
}
