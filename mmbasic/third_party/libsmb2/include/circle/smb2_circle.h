/*
 * BSD socket surface for the Circle port of libsmb2 (#1132).
 *
 * Circle has no POSIX/BSD socket headers, so this supplies the types, the
 * constants and the function prototypes the vendored library references. The
 * implementations live in console/smb2_sock.cpp on top of Circle CSocket. It
 * is included from lib/compat.h under #ifdef __circle__, so it is visible to
 * every libsmb2 translation unit; console/smb2_sock.cpp includes it directly
 * so the definitions see the same declarations.
 *
 * Only the IPv4 client surface is provided. The server helpers in libsmb2
 * (bind/listen/accept/select) are stubbed because mmcore only ever connects
 * as a client.
 */
#ifndef LIBSMB2_MMCORE_CIRCLE_H
#define LIBSMB2_MMCORE_CIRCLE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif
#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#endif
#ifndef SO_LINGER
#define SO_LINGER 13
#endif
#ifndef SO_ERROR
#define SO_ERROR 4
#endif
#ifndef SOL_TCP
#define SOL_TCP 6
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif
#ifndef INADDR_ANY
#define INADDR_ANY ((uint32_t)0)
#endif

/* getaddrinfo() error codes (no <netdb.h> on Circle). The values only have to
 * be distinct and non-zero. */
#ifndef EAI_AGAIN
#define EAI_AGAIN 2
#endif
#ifndef EAI_FAIL
#define EAI_FAIL 4
#endif
#ifndef EAI_MEMORY
#define EAI_MEMORY 6
#endif
#ifndef EAI_NONAME
#define EAI_NONAME 8
#endif
#ifndef EAI_SERVICE
#define EAI_SERVICE 9
#endif

/* Poll events (match lib/compat.h's values so the library and the shim agree). */
#ifndef POLLIN
#define POLLIN 0x0001
#endif
#ifndef POLLPRI
#define POLLPRI 0x0002
#endif
#ifndef POLLOUT
#define POLLOUT 0x0004
#endif
#ifndef POLLERR
#define POLLERR 0x0008
#endif
#ifndef POLLHUP
#define POLLHUP 0x0010
#endif

/* Little-endian aarch64. */
#define htons(x) ((uint16_t)__builtin_bswap16 ((uint16_t)(x)))
#define ntohs(x) htons (x)
#define htonl(x) ((uint32_t)__builtin_bswap32 ((uint32_t)(x)))
#define ntohl(x) htonl (x)

typedef uint32_t socklen_t;

struct sockaddr {
	unsigned short sa_family;
	char sa_data[14];
};

struct in_addr {
	uint32_t s_addr;
};

struct sockaddr_in {
	unsigned short sin_family;
	unsigned short sin_port;
	struct in_addr sin_addr;
	char sin_zero[8];
};

struct sockaddr_storage {
	unsigned short ss_family;
	char ss_pad[126];
};

struct iovec {
	void *iov_base;
	size_t iov_len;
};

struct pollfd {
	int fd;
	short events;
	short revents;
};

/*
 * ai_priv is private to this port: it points at the allocation that backs
 * ai_addr so freeaddrinfo() can release it. libsmb2 only touches the standard
 * fields.
 */
struct addrinfo {
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	size_t ai_addrlen;
	char *ai_canonname;
	struct sockaddr *ai_addr;
	struct addrinfo *ai_next;
	void *ai_priv;
};

int socket (int nDomain, int nType, int nProtocol);
int connect (int nSocket, const struct sockaddr *pAddress, socklen_t nAddressLength);
int bind (int nSocket, const struct sockaddr *pAddress, socklen_t nAddressLength);
int listen (int nSocket, int nBacklog);
int accept (int nSocket, struct sockaddr *pAddress, socklen_t *pAddressLength);
int close (int nSocket);
ssize_t readv (int nSocket, const struct iovec *pIOVectors, int nVectorCount);
ssize_t writev (int nSocket, const struct iovec *pIOVectors, int nVectorCount);
int poll (struct pollfd *pFDs, unsigned int nFDCount, int nTimeoutMs);
int setsockopt (int nSocket, int nLevel, int nOption, const void *pValue,
		socklen_t nValueLength);
int getsockopt (int nSocket, int nLevel, int nOption, void *pValue,
		socklen_t *pValueLength);
int fcntl (int nSocket, int nCommand, ...);
int getaddrinfo (const char *pNode, const char *pService,
		 const struct addrinfo *pHints, struct addrinfo **ppResult);
void freeaddrinfo (struct addrinfo *pResult);

/* libc helpers implemented in console/smb2_sock.cpp. Declared here so a
 * libsmb2 translation unit that does not pull in the newlib header still sees
 * a prototype (GCC 15 rejects implicit declarations). */
long random (void);
void srandom (unsigned int nSeed);
int getpid (void);
int gethostname (char *pName, size_t nLength);
int getlogin_r (char *pName, size_t nLength);
char *getenv (const char *pName);
char *strerror (int nError);
long strtol (const char *pString, char **ppEnd, int nBase);
char *strdup (const char *pString);

#ifdef __cplusplus
}
#endif

#endif /* LIBSMB2_MMCORE_CIRCLE_H */
