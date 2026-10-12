/*
 * Circle SMB adapter (#1132).
 *
 * The shared libsmb2 backend (mmbasic/src/netfs_smb2.c) talks to a small BSD
 * socket surface. On bare-metal Circle there are no POSIX sockets, no poll()
 * and a very small libc, so this file provides both:
 *
 *  - a dedicated fd table over Circle CSocket (never the shared TERM/CONNECT
 *    client in console/net.cpp). socket() allocates a slot, connect() starts a
 *    CTask that resolves the name and connects (returning EINPROGRESS), and
 *    mmb_plat_poll_fd() reports POLLOUT once connected and POLLIN when a
 *    DONTWAIT Receive() burst into the slot's dedicated 32 KB receive buffer
 *    (SMB_RX_BUFFER_SIZE, independent of the 1600-byte net frame) found data.
 *    readv()/writev() loop over Receive()/Send() and close() deletes the
 *    socket.
 *
 *  - the libc helpers libsmb2 needs: time() from CTimer (NTP-synced),
 *    random() from the BCM hardware RNG, snprintf()/vsnprintf()/asprintf()
 *    over mmb_vsnprintf(), strdup(), strtol(), strerror() and the small
 *    getpid()/gethostname()/getlogin_r()/getenv() stubs.
 *
 * The wait loop in netfs_smb2.c calls mmb_poll() and checks Ctrl-C between
 * poll waits, so Circle's network tasks keep running while an SMB op is in
 * flight.
 */
#ifdef MMB_CIRCLE_NET

#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/net/in.h>
#include <circle/net/dnsclient.h>
#include <circle/net/ipaddress.h>
#include <circle/net/error.h>
#include <circle/netdevice.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/timer.h>
#include <circle/bcmrandom.h>
#include <circle/util.h>

#include <circle/smb2_circle.h>

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

/* From console/net.cpp (Circle network subsystem, if one is up). */
CNetSubSystem *mmb_circle_net (void);

/* Shared mmbasic helpers (C linkage). Note: mmb_priv.h is not included here
 * because it is not wrapped in extern "C" and this is a C++ translation unit. */
extern "C" unsigned mmb_now_ms (void);
extern "C" int mmb_vsnprintf (char *str, unsigned long size, const char *fmt,
			      va_list ap);

#define SMB_MAX_FD 8

/* Dedicated receive buffer for the SMB sockets (#1144). FRAME_BUFFER_SIZE is
 * the 1600-byte net frame size; a single CSocket::Receive() returns only one
 * reassembled TCP frame (<= ~1460 bytes), so a 64 KB SMB2 READ response would
 * otherwise take dozens of readv()/poll() round-trips, each with a scheduler
 * yield. This larger buffer lets one readiness check / readv() drain a whole
 * burst of frames first. SMB_MAX_FD slots are allocated once and live in BSS;
 * 32 KB x 8 = 256 KB, modest on the 512 MB Pi Zero 2. */
#define SMB_RX_BUFFER_SIZE (32u * 1024u)

enum smb_fd_state
{
	SMB_FD_FREE = 0,
	SMB_FD_ALLOC,
	SMB_FD_CONNECTING,
	SMB_FD_CONNECTED,
	SMB_FD_FAILED
};

struct smb_fd
{
	int used;
	int state;
	unsigned gen;
	char host[160];
	unsigned short port;
	CSocket *sock;
	int connect_rc;
	unsigned char rx[SMB_RX_BUFFER_SIZE];
	unsigned rxn;
	unsigned rxoff;
	int peer_closed;
};

static struct smb_fd s_fds[SMB_MAX_FD];
static unsigned s_gen_counter;
/* getaddrinfo() has no slot to hang the hostname on (libsmb2 resolves before
 * it opens the socket and copies only sockaddr_in), so stash it here for the
 * following connect(). libsmb2 is single-threaded and mmcore connects one
 * share at a time. */
static char s_pending_host[160];
static unsigned short s_pending_port = 445;

static struct smb_fd *fd_slot (int fd)
{
	if (fd < 0 || fd >= SMB_MAX_FD || !s_fds[fd].used)
		return 0;
	return &s_fds[fd];
}

/* Top up the slot's receive buffer from the socket without blocking. Each
 * CSocket::Receive() returns at most one TCP frame, so loop until the buffer
 * is full, the socket would block (0), or it errors/closes (<0). Leftover
 * bytes are compacted to the front first. Returns the bytes now buffered. */
static int smb_fd_drain (struct smb_fd *s)
{
	unsigned avail = s->rxn - s->rxoff;

	if (avail && s->rxoff)
		memmove (s->rx, s->rx + s->rxoff, avail);
	s->rxn = avail;
	s->rxoff = 0;

	while (s->rxn < SMB_RX_BUFFER_SIZE)
	{
		int n = s->sock->Receive (s->rx + s->rxn,
					  SMB_RX_BUFFER_SIZE - s->rxn,
					  MSG_DONTWAIT);
		if (n > 0)
		{
			s->rxn += (unsigned) n;
			continue;
		}
		if (n < 0)
			s->peer_closed = 1;
		break;
	}
	return (int) (s->rxn - s->rxoff);
}

/* Report the readiness of one slot. May consume TCP frames into the leftover
 * buffer. 0 means "nothing yet". */
static int smb_fd_check (int fd, int events)
{
	struct smb_fd *s;
	int revents = 0;

	if (fd < 0 || fd >= SMB_MAX_FD || !s_fds[fd].used)
		return -1;
	s = &s_fds[fd];
	if (s->state == SMB_FD_CONNECTING)
		return 0;
	if (s->state != SMB_FD_CONNECTED)
		return POLLERR;
	if (events & POLLOUT)
		revents |= POLLOUT;
	if (events & POLLIN)
	{
		if (s->rxoff < s->rxn)
			revents |= POLLIN;
		else if (s->peer_closed)
			revents |= POLLIN;	/* let readv() report EOF */
		else
		{
			int avail = smb_fd_drain (s);

			if (CScheduler::IsActive ())
				CScheduler::Get ()->Yield ();
			if (avail > 0)
				revents |= POLLIN;
			else if (s->peer_closed)
				revents |= POLLIN;
		}
	}
	return revents;
}

/* Connect runs on its own task: CSocket::Connect() blocks until the SYN
 * completes (or the TCP retransmits give up), and name resolution can take a
 * moment, so it must not run on the interpreter task. */
class CSmbConnectTask : public CTask
{
public:
	CSmbConnectTask (int fd, unsigned gen)
	:	CTask (TASK_STACK_SIZE),
		m_fd (fd),
		m_gen (gen)
	{
	}

	void Run (void)
	{
		struct smb_fd *s = &s_fds[m_fd];
		CNetSubSystem *net = mmb_circle_net ();
		CIPAddress ip;
		CSocket *sock;
		int rc;

		if (!net || !net->IsRunning ())
		{
			Finish (-1);
			return;
		}
		{
			CDNSClient dns (net);
			if (!dns.Resolve (s->host, &ip))
			{
				Finish (-2);
				return;
			}
		}
		sock = new CSocket (net, IPPROTO_TCP);
		if (!sock)
		{
			Finish (-3);
			return;
		}
		rc = sock->Connect (ip, s->port);
		if (rc < 0)
		{
			delete sock;
			Finish (rc);
			return;
		}
		if (s->gen != m_gen || s->state != SMB_FD_CONNECTING)
		{
			/* The slot was closed or reused while we were connecting. */
			delete sock;
			return;
		}
		s->sock = sock;
		s->state = SMB_FD_CONNECTED;
	}

private:
	void Finish (int rc)
	{
		struct smb_fd *s = &s_fds[m_fd];

		if (s->gen == m_gen && s->state == SMB_FD_CONNECTING)
		{
			s->connect_rc = rc;
			s->state = SMB_FD_FAILED;
		}
	}

	int m_fd;
	unsigned m_gen;
};

extern "C" {

/* ---- fd table over CSocket ------------------------------------------ */

int socket (int nDomain, int nType, int nProtocol)
{
	int i;

	(void) nProtocol;
	if (nDomain != AF_INET || nType != SOCK_STREAM)
	{
		errno = EAFNOSUPPORT;
		return -1;
	}
	for (i = 0; i < SMB_MAX_FD; i++)
		if (!s_fds[i].used)
			break;
	if (i == SMB_MAX_FD)
	{
		errno = ENOMEM;
		return -1;
	}
	memset (&s_fds[i], 0, sizeof s_fds[i]);
	s_fds[i].used = 1;
	s_fds[i].state = SMB_FD_ALLOC;
	s_fds[i].gen = ++s_gen_counter;
	return i;
}

int connect (int nSocket, const struct sockaddr *pAddress, socklen_t nLength)
{
	struct smb_fd *s;
	size_t n;

	(void) pAddress;
	(void) nLength;
	s = fd_slot (nSocket);
	if (!s)
	{
		errno = EBADF;
		return -1;
	}
	if (s->state != SMB_FD_ALLOC)
	{
		errno = EISCONN;
		return -1;
	}
	if (!CScheduler::IsActive ())
	{
		errno = ENETDOWN;
		return -1;
	}
	n = strlen (s_pending_host);
	if (n > sizeof s->host - 1)
		n = sizeof s->host - 1;
	memcpy (s->host, s_pending_host, n);
	s->host[n] = 0;
	if (!s->host[0])
	{
		errno = EDESTADDRREQ;
		return -1;
	}
	s->port = s_pending_port;
	s->state = SMB_FD_CONNECTING;
	new CSmbConnectTask (nSocket, s->gen);
	errno = EINPROGRESS;
	return -1;
}

int bind (int nSocket, const struct sockaddr *pAddress, socklen_t nLength)
{
	(void) nSocket;
	(void) pAddress;
	(void) nLength;
	errno = EOPNOTSUPP;
	return -1;
}

int listen (int nSocket, int nBacklog)
{
	(void) nSocket;
	(void) nBacklog;
	errno = EOPNOTSUPP;
	return -1;
}

int accept (int nSocket, struct sockaddr *pAddress, socklen_t *pLength)
{
	(void) nSocket;
	(void) pAddress;
	(void) pLength;
	errno = EOPNOTSUPP;
	return -1;
}

int close (int nSocket)
{
	struct smb_fd *s = &s_fds[0];

	if (nSocket < 0 || nSocket >= SMB_MAX_FD || !s_fds[nSocket].used)
	{
		errno = EBADF;
		return -1;
	}
	s = &s_fds[nSocket];
	if (s->sock)
	{
		delete s->sock;
		s->sock = 0;
	}
	s->used = 0;
	s->state = SMB_FD_FREE;
	s->gen++;			/* invalidate an in-flight connect task */
	return 0;
}

ssize_t readv (int nSocket, const struct iovec *pVectors, int nVectorCount)
{
	struct smb_fd *s = fd_slot (nSocket);
	ssize_t total = 0;
	int i;

	if (!s)
	{
		errno = EBADF;
		return -1;
	}
	if (s->state != SMB_FD_CONNECTED)
	{
		errno = ENOTCONN;
		return -1;
	}
	if (s->rxoff >= s->rxn)
	{
		int avail;

		if (s->peer_closed)
			return 0;
		avail = smb_fd_drain (s);
		if (CScheduler::IsActive ())
			CScheduler::Get ()->Yield ();
		if (avail <= 0)
		{
			if (s->peer_closed)
				return 0;
			errno = EAGAIN;
			return -1;
		}
	}
	for (i = 0; i < nVectorCount && s->rxoff < s->rxn; i++)
	{
		unsigned avail = s->rxn - s->rxoff;
		unsigned want = (unsigned) pVectors[i].iov_len;
		unsigned n = avail < want ? avail : want;

		if (!n)
			continue;
		memcpy (pVectors[i].iov_base, s->rx + s->rxoff, n);
		s->rxoff += n;
		total += (ssize_t) n;
	}
	return total;
}

ssize_t writev (int nSocket, const struct iovec *pVectors, int nVectorCount)
{
	struct smb_fd *s = fd_slot (nSocket);
	ssize_t total = 0;
	int i;

	if (!s)
	{
		errno = EBADF;
		return -1;
	}
	if (s->state != SMB_FD_CONNECTED)
	{
		errno = ENOTCONN;
		return -1;
	}
	for (i = 0; i < nVectorCount; i++)
	{
		unsigned want = (unsigned) pVectors[i].iov_len;
		int rc;

		if (!want)
			continue;
		rc = s->sock->Send (pVectors[i].iov_base, want, MSG_DONTWAIT);
		if (CScheduler::IsActive ())
			CScheduler::Get ()->Yield ();
		if (rc < 0)
		{
			if (total > 0)
				return total;
			s->peer_closed = 1;
			errno = EIO;
			return -1;
		}
		total += rc;
		if ((unsigned) rc < want)
			break;		/* report progress; libsmb2 retries */
	}
	return total;
}

int poll (struct pollfd *pFDs, unsigned int nFDCount, int nTimeoutMs)
{
	unsigned start = mmb_now_ms ();
	unsigned i;
	int ready = 0;

	if (!pFDs || !nFDCount)
		return 0;
	for (;;)
	{
		ready = 0;
		for (i = 0; i < nFDCount; i++)
		{
			int r;

			pFDs[i].revents = 0;
			if (pFDs[i].fd < 0)
				continue;
			r = smb_fd_check (pFDs[i].fd, pFDs[i].events);
			if (r < 0)
				pFDs[i].revents = POLLERR;
			else if (r)
				pFDs[i].revents = (short) r;
			if (pFDs[i].revents)
				ready++;
		}
		if (ready || nTimeoutMs == 0)
			return ready;
		if (nTimeoutMs > 0 &&
		    (int) (mmb_now_ms () - start) >= nTimeoutMs)
			return 0;
		if (CScheduler::IsActive ())
			CScheduler::Get ()->MsSleep (2);
		else
			CTimer::SimpleMsDelay (2);
	}
}

int select (int nFDCount, fd_set *pRead, fd_set *pWrite, fd_set *pExcept,
	    struct timeval *pTimeout)
{
	(void) nFDCount;
	(void) pRead;
	(void) pWrite;
	(void) pExcept;
	(void) pTimeout;
	errno = EOPNOTSUPP;
	return -1;
}

int setsockopt (int nSocket, int nLevel, int nOption, const void *pValue,
		socklen_t nValueLength)
{
	(void) nSocket;
	(void) nLevel;
	(void) nOption;
	(void) pValue;
	(void) nValueLength;
	return 0;
}

int getsockopt (int nSocket, int nLevel, int nOption, void *pValue,
		socklen_t *pValueLength)
{
	struct smb_fd *s = fd_slot (nSocket);

	(void) nLevel;
	(void) nOption;
	if (pValue && pValueLength && *pValueLength >= sizeof (int))
	{
		int err = 0;

		if (!s || s->state != SMB_FD_CONNECTED)
			err = (s && s->connect_rc) ? s->connect_rc : -1;
		*(int *) pValue = err;
		*pValueLength = sizeof (int);
	}
	return 0;
}

int fcntl (int nSocket, int nCommand, ...)
{
	(void) nSocket;
	(void) nCommand;
	return 0;		/* sockets are non-blocking by construction */
}

int getaddrinfo (const char *pNode, const char *pService,
		 const struct addrinfo *pHints, struct addrinfo **ppResult)
{
	struct addrinfo *ai;
	struct sockaddr_in *sin;

	(void) pHints;
	if (!pNode || !pNode[0] || !ppResult)
		return EAI_NONAME;
	sin = (struct sockaddr_in *) malloc (sizeof *sin);
	if (!sin)
		return EAI_MEMORY;
	memset (sin, 0, sizeof *sin);
	sin->sin_family = AF_INET;
	sin->sin_port = htons ((uint16_t) atoi (pService ? pService : "445"));
	{
		size_t n = strlen (pNode);

		if (n > sizeof s_pending_host - 1)
			n = sizeof s_pending_host - 1;
		memcpy (s_pending_host, pNode, n);
		s_pending_host[n] = 0;
	}
	s_pending_port = atoi (pService ? pService : "445");
	ai = (struct addrinfo *) malloc (sizeof *ai);
	if (!ai)
	{
		free (sin);
		return EAI_MEMORY;
	}
	memset (ai, 0, sizeof *ai);
	ai->ai_family = AF_INET;
	ai->ai_socktype = SOCK_STREAM;
	ai->ai_addrlen = sizeof (struct sockaddr_in);
	ai->ai_addr = (struct sockaddr *) sin;
	ai->ai_priv = sin;
	*ppResult = ai;
	return 0;
}

void freeaddrinfo (struct addrinfo *pResult)
{
	struct addrinfo *ai = pResult;

	while (ai)
	{
		struct addrinfo *next = ai->ai_next;

		free (ai->ai_priv);
		free (ai);
		ai = next;
	}
}

/* ---- the single platform hook netfs_smb2.c needs -------------------- */

int mmb_plat_poll_fd (intptr_t fd, int events, int timeout_ms)
{
	unsigned start = mmb_now_ms ();
	int r;
	int slot = (int) fd;

	if (slot < 0 || slot >= SMB_MAX_FD || !s_fds[slot].used)
		return -1;
	for (;;)
	{
		r = smb_fd_check (slot, events);
		if (r)
			return r;
		if (timeout_ms <= 0 ||
		    (int) (mmb_now_ms () - start) >= timeout_ms)
			return 0;
		if (CScheduler::IsActive ())
			CScheduler::Get ()->MsSleep (2);
		else
			CTimer::SimpleMsDelay (2);
	}
}

/* ---- libc shims ----------------------------------------------------- */

time_t time (time_t *pTloc)
{
	CTimer *t = CTimer::Get ();
	unsigned sec = 0, usec = 0;

	if (t)
		t->GetUniversalTime (&sec, &usec);
	if (pTloc)
		*pTloc = (time_t) sec;
	return (time_t) sec;
}

long random (void)
{
	static CBcmRandomNumberGenerator *rng;

	if (!rng)
		rng = new CBcmRandomNumberGenerator ();
	return rng ? (long) rng->GetNumber () : 0;
}

void srandom (unsigned int seed)
{
	(void) seed;
}

int getpid (void)
{
	return 1;
}

int gethostname (char *pName, size_t nLength)
{
	if (pName && nLength)
	{
		const char *name = "mmcore";
		size_t i = 0;

		while (name[i] && i + 1 < nLength)
		{
			pName[i] = name[i];
			i++;
		}
		pName[i] = 0;
	}
	return 0;
}

int getlogin_r (char *pName, size_t nLength)
{
	(void) pName;
	(void) nLength;
	return ENXIO;
}

char *getenv (const char *pName)
{
	(void) pName;
	return 0;
}

char *strerror (int nError)
{
	switch (nError)
	{
	case 0:			return (char *) "no error";
	case EINPROGRESS:	return (char *) "in progress";
	case EAGAIN:		return (char *) "try again";
	case ECONNREFUSED:	return (char *) "connection refused";
	case ETIMEDOUT:		return (char *) "timed out";
	case ECONNRESET:	return (char *) "connection reset";
	case ENETUNREACH:	return (char *) "network unreachable";
	case EHOSTUNREACH:	return (char *) "host unreachable";
	case ENOTCONN:		return (char *) "not connected";
	case EBADF:		return (char *) "bad file descriptor";
	case ENOMEM:		return (char *) "out of memory";
	default:		return (char *) "error";
	}
}

long strtol (const char *pString, char **ppEnd, int nBase)
{
	const char *p = pString;
	long value = 0;
	int neg = 0, any = 0;

	if (!p)
	{
		if (ppEnd)
			*ppEnd = 0;
		return 0;
	}
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
	       *p == '\f' || *p == '\v')
		p++;
	if (*p == '+')
		p++;
	else if (*p == '-')
	{
		neg = 1;
		p++;
	}
	if ((nBase == 0 || nBase == 16) && p[0] == '0' &&
	    (p[1] == 'x' || p[1] == 'X'))
	{
		nBase = 16;
		p += 2;
	}
	else if (nBase == 0 && p[0] == '0')
	{
		nBase = 8;
		p++;
	}
	else if (nBase == 0)
		nBase = 10;
	for (;; p++)
	{
		int c = (unsigned char) *p;
		int d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'z')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z')
			d = c - 'A' + 10;
		else
			break;
		if (d >= nBase)
			break;
		value = value * nBase + d;
		any = 1;
	}
	if (ppEnd)
		*ppEnd = (char *) (any ? p : pString);
	return neg ? -value : value;
}

char *strdup (const char *pString)
{
	size_t n;
	char *p;

	if (!pString)
		return 0;
	n = strlen (pString) + 1;
	p = (char *) malloc (n);
	if (p)
		memcpy (p, pString, n);
	return p;
}

int vsnprintf (char *pStr, size_t nSize, const char *pFormat, va_list ap)
{
	return mmb_vsnprintf (pStr, (unsigned long) nSize, pFormat, ap);
}

int snprintf (char *pStr, size_t nSize, const char *pFormat, ...)
{
	va_list ap;
	int r;

	va_start (ap, pFormat);
	r = mmb_vsnprintf (pStr, (unsigned long) nSize, pFormat, ap);
	va_end (ap);
	return r;
}

int asprintf (char **ppStr, const char *pFormat, ...)
{
	va_list ap;
	char *buf;
	int n;

	va_start (ap, pFormat);
	n = mmb_vsnprintf (0, 0, pFormat, ap);
	va_end (ap);
	if (n < 0)
	{
		*ppStr = 0;
		return -1;
	}
	buf = (char *) malloc ((size_t) n + 1);
	if (!buf)
	{
		*ppStr = 0;
		return -1;
	}
	va_start (ap, pFormat);
	mmb_vsnprintf (buf, (unsigned long) n + 1, pFormat, ap);
	va_end (ap);
	*ppStr = buf;
	return n;
}

/* stdio stub pulled in by libsmb2's optional NTLM_USER_FILE path and a debug
 * helper; it never runs because getenv() returns NULL. fopen()/fclose() are
 * already stubbed for jar_xm in mmbasic/src/cmd_play.c, so only fgets() is
 * needed here. */
int printf (const char *pFormat, ...)
{
	(void) pFormat;
	return 0;
}

char *fgets (char *pStr, int nSize, FILE *pFile)
{
	(void) pStr;
	(void) nSize;
	(void) pFile;
	return 0;
}

/*
 * newlib's <ctype.h> macros (used by ntlmssp.c's toupper()) index the
 * `_ctype_` class table, which newlib itself would define. Circle does not
 * link newlib, so supply the standard ASCII layout here. Index 0 is unused;
 * character c lives at _ctype_[c + 1].
 */
extern const char _ctype_[257];
const char _ctype_[257] = {
	0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, -0x58, 0x28, 0x28, 0x28, 0x28, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, -0x78, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x10, 0x10, 0x10, 0x10,
	0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00,
};

} /* extern "C" */

#endif /* MMB_CIRCLE_NET */
