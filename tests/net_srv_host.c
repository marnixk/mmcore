/* Host test for the POSIX TCP server contract (linux/net_posix.c).
 *
 * Single process: listen, connect a client to it, accept, exchange bytes, and
 * check the non-blocking send/recv semantics the FTP server relies on.
 */
/* mmb_priv.h declares sprintf(); include it before <stdio.h>, whose macOS
 * macro form would otherwise clash with the prototype. */
#include "mmb_priv.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

unsigned mmb_now_ms(void) { return 0; }

int main(void)
{
	int lsn, port, cf, conn = -1, n, i;
	struct sockaddr_in a;
	char buf[16];
	const char *ping = "PING";

	lsn = mmb_net_srv_listen(0);
	if (lsn < 0)
	{
		printf("server listen failed\n");
		return 1;
	}
	port = mmb_net_srv_port(lsn);
	if (port <= 0)
	{
		printf("server port unknown\n");
		return 1;
	}

	cf = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(cf, (struct sockaddr *)&a, sizeof a) != 0)
	{
		printf("client connect failed\n");
		return 1;
	}

	for (i = 0; i < 500 && conn < 0; i++)
	{
		conn = mmb_net_srv_accept(lsn);
		if (conn < 0)
			usleep(1000);
	}
	if (conn < 0)
	{
		printf("server accept failed\n");
		return 1;
	}
	if (send(cf, ping, 4, 0) != 4)
	{
		printf("client send failed\n");
		return 1;
	}

	n = 0;
	for (i = 0; i < 500 && n == 0; i++)
	{
		n = mmb_net_srv_recv(conn, buf, sizeof buf);
		if (n == 0)
			usleep(1000);
	}
	if (n != 4 || memcmp(buf, "PING", 4) != 0)
	{
		printf("server recv failed (n=%d)\n", n);
		return 1;
	}

	n = 0;
	for (i = 0; i < 500 && n == 0; i++)
	{
		n = mmb_net_srv_send(conn, "PONG", 4);
		if (n == 0)
			usleep(1000);
	}
	if (n != 4)
	{
		printf("server send failed (n=%d)\n", n);
		return 1;
	}

	n = recv(cf, buf, 4, 0);
	if (n != 4 || memcmp(buf, "PONG", 4) != 0)
	{
		printf("client recv failed (n=%d)\n", n);
		return 1;
	}

	if (mmb_net_srv_closed(conn))
	{
		printf("server reported a live connection closed\n");
		return 1;
	}
	mmb_net_srv_close(conn);
	mmb_net_srv_listen_close(lsn);
	close(cf);
	printf("all checks passed\n");
	return 0;
}
