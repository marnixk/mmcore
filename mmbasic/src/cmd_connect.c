#include "mmb_priv.h"

#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define SE    240
#define TELOPT_ECHO  1
#define TELOPT_SGA   3
#define TELOPT_TTYPE 24
#define TTYPE_IS     0
#define TTYPE_SEND   1

#define CN_LINE  256

typedef struct {
	int active;
	int char_mode;
	int no_echo;
	int iac;
	int iac_cmd;
	int sb;
	int last_eol;
	int sb_opt;
	int sb_cmd;
	char host[80];
	int port;
	char line[CN_LINE];
	int linelen;
} cn_state;

static cn_state C;

static void emit_scr(const char *s, unsigned n)
{
	if (s && n && G.plat && G.plat->write_screen)
		G.plat->write_screen(s, n);
}

static void emit_ser(const char *s, unsigned n)
{
	if (s && n && G.plat && G.plat->write_serial)
		G.plat->write_serial(s, n);
}

static void emit_both(const char *s, unsigned n)
{
	emit_scr(s, n);
	emit_ser(s, n);
}

static void send_iac(int cmd, int opt)
{
	unsigned char b[3];
	b[0] = IAC;
	b[1] = (unsigned char)cmd;
	b[2] = (unsigned char)opt;
	mmb_net_tcp_send(b, 3);
}

static void send_ttype(void)
{
	static const unsigned char ttype[] = {
		IAC, SB, TELOPT_TTYPE, TTYPE_IS,
		'A', 'N', 'S', 'I',
		IAC, SE
	};
	mmb_net_tcp_send(ttype, (unsigned)sizeof(ttype));
}

static void apply_option(int cmd, int opt)
{
	if (opt == TELOPT_ECHO)
	{
		if (cmd == WILL)
		{
			C.no_echo = 1;
			send_iac(DO, TELOPT_ECHO);
		}
		else if (cmd == WONT)
		{
			C.no_echo = 0;
			send_iac(DONT, TELOPT_ECHO);
		}
		else if (cmd == DO)
			send_iac(WONT, TELOPT_ECHO);
		else
			send_iac(WONT, TELOPT_ECHO);
		return;
	}
	if (opt == TELOPT_SGA)
	{
		if (cmd == WILL)
		{
			C.char_mode = 1;
			send_iac(DO, TELOPT_SGA);
		}
		else if (cmd == WONT)
		{
			C.char_mode = 0;
			send_iac(DONT, TELOPT_SGA);
		}
		else if (cmd == DO)
		{
			C.char_mode = 1;
			send_iac(WILL, TELOPT_SGA);
		}
		else
		{
			C.char_mode = 0;
			send_iac(WONT, TELOPT_SGA);
		}
		return;
	}
	if (opt == TELOPT_TTYPE)
	{
		if (cmd == DO)
		{
			send_iac(WILL, TELOPT_TTYPE);
			send_ttype();
		}
		else if (cmd == WILL)
			send_iac(DONT, TELOPT_TTYPE);
		else
			send_iac(WONT, TELOPT_TTYPE);
		return;
	}
	if (cmd == WILL)
		send_iac(DONT, opt);
	else if (cmd == DO)
		send_iac(WONT, opt);
}

static void incoming_byte(unsigned char b)
{
	if (C.sb)
	{
		if (C.iac == 1)
		{
			C.iac = 0;
			if (b == SE)
			{
				if (C.sb_opt == TELOPT_TTYPE && C.sb_cmd == TTYPE_SEND)
					send_ttype();
				C.sb = 0;
			}
		}
		else if (b == IAC)
			C.iac = 1;
		else if (C.sb_opt < 0)
			C.sb_opt = (int)b;
		else if (C.sb_cmd < 0)
			C.sb_cmd = (int)b;
		return;
	}
	if (C.iac == 1)
	{
		if (b == IAC)
		{
			C.iac = 0;
			emit_both((const char *)&b, 1);
			return;
		}
		if (b == SB)
		{
			C.iac = 0;
			C.sb = 1;
			C.sb_opt = -1;
			C.sb_cmd = -1;
			return;
		}
		if (b == WILL || b == WONT || b == DO || b == DONT)
		{
			C.iac_cmd = b;
			C.iac = 2;
			return;
		}
		C.iac = 0;
		return;
	}
	if (C.iac == 2)
	{
		apply_option(C.iac_cmd, b);
		C.iac = 0;
		return;
	}
	if (b == IAC)
	{
		C.iac = 1;
		return;
	}
	if (b == 0)
		return;
	{
		char c = (char)b;
		emit_both(&c, 1);
	}
}

static void session_close(const char *why)
{
	mmb_net_tcp_close();
	C.active = 0;
	C.linelen = 0;
	if (why && why[0])
		emit_both(why, (unsigned)strlen(why));
}

static void send_line(void)
{
	C.line[C.linelen] = 0;
	if (C.linelen)
		mmb_net_tcp_send(C.line, (unsigned)C.linelen);
	mmb_net_tcp_send("\r\n", 2);
	C.linelen = 0;
}

void mmb_cmd_connect(void)
{
	mmb_val host, portv;
	int port;

	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		mmb_syntax();
	host = mmb_expr();
	if (host.type != T_STR)
		mmb_syntax();
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		mmb_syntax();
	portv = mmb_expr();
	if (portv.type != T_INT && portv.type != T_NUM)
		mmb_syntax();
	port = (int)mmb_as_int(portv);
	if (port < 1 || port > 65535)
		mmb_error("?SYNTAX ERROR");

	strncpy(C.host, host.s, sizeof(C.host) - 1);
	C.host[sizeof(C.host) - 1] = 0;
	C.port = port;
	C.char_mode = 0;
	C.no_echo = 0;
	C.iac = 0;
	C.sb = 0;
	C.last_eol = 0;
	C.sb_opt = -1;
	C.sb_cmd = -1;
	C.linelen = 0;

	if (mmb_net_tcp_open(C.host, C.port) != 0)
	{
		if (!mmb_net_available())
			mmb_out("Network not available");
		else
			mmb_out("Connect failed");
		return;
	}
	C.active = 1;
	mmb_out("Connected. Ctrl+] to quit.");
}

int mmb_in_connect(void)
{
	return C.active;
}

static int swallow_crlf_pair(char c)
{
	if (c != '\r' && c != '\n')
	{
		C.last_eol = 0;
		return 0;
	}
	if (C.last_eol && C.last_eol != (unsigned char)c)
	{
		C.last_eol = 0;
		return 1;
	}
	C.last_eol = (unsigned char)c;
	return 0;
}

static void send_enter(void)
{
	mmb_net_tcp_send("\r\n", 2);
	if (!C.no_echo)
		emit_both("\n", 1);
}

const char *mmb_connect_key(char c)
{
	unsigned char b;

	if (!C.active)
		return "";
	if ((unsigned char)c == 0x1d) /* Ctrl+] telnet escape */
	{
		session_close("\r\nConnection closed\r\n");
		return "";
	}
	if (swallow_crlf_pair(c))
		return "";
	if (C.char_mode)
	{
		if (c == '\r' || c == '\n')
		{
			send_enter();
			return "";
		}
		b = (unsigned char)c;
		if (b == IAC)
		{
			unsigned char esc[2] = { IAC, IAC };
			mmb_net_tcp_send(esc, 2);
		}
		else
			mmb_net_tcp_send(&b, 1);
		if (!C.no_echo)
			emit_both(&c, 1);
		return "";
	}
	if (c == '\r' || c == '\n')
	{
		if (!C.no_echo)
			emit_both("\n", 1);
		send_line();
		return "";
	}
	if (c == 8 || c == 127)
	{
		if (C.linelen > 0)
		{
			C.linelen--;
			if (!C.no_echo)
				emit_both("\b \b", 3);
		}
		return "";
	}
	if ((unsigned char)c < 32)
		return "";
	if (C.linelen + 1 < CN_LINE)
	{
		C.line[C.linelen++] = c;
		if (!C.no_echo)
			emit_both(&c, 1);
	}
	return "";
}

void mmb_connect_poll(void)
{
	unsigned char buf[512];
	int n, i, loops;

	if (!C.active)
		return;
	for (loops = 0; loops < 32; loops++)
	{
		n = mmb_net_tcp_recv(buf, sizeof(buf));
		if (n < 0)
		{
			session_close("\r\nConnection closed\r\n");
			return;
		}
		if (n == 0)
			return;
		for (i = 0; i < n; i++)
			incoming_byte(buf[i]);
	}
}

void mmb_cmd_ipconfig(void)
{
	char buf[512];

	if (mmb_wlan_ipconfig(buf, (int)sizeof(buf)) != 0 && !buf[0])
		mmb_out("Wi-Fi not available");
	else
		mmb_out(buf);
}
