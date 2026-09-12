#include "mmb_priv.h"

static int at_end(void)
{
	mmb_skip_sp();
	return *G.p == 0 || *G.p == ':' || *G.p == '\'';
}

static void read_topic(char *dst, int dstsz)
{
	int n = 0;
	mmb_skip_sp();
	if (*G.p == '?')
	{
		dst[0] = '?';
		dst[1] = 0;
		G.p++;
		return;
	}
	while (*G.p && *G.p != ':' && *G.p != '\'' && n < dstsz - 2)
	{
		char tok[48];
		if (*G.p == ' ' || *G.p == '\t')
		{
			mmb_skip_sp();
			if (*G.p && *G.p != ':' && *G.p != '\'' && n > 0 && dst[n - 1] != ' ')
				dst[n++] = ' ';
			continue;
		}
		if (mmb_tok_expand(tok, (int)sizeof(tok)))
		{
			int i;
			for (i = 0; tok[i] && n < dstsz - 2; i++)
				dst[n++] = tok[i];
			continue;
		}
		{
			char c = *G.p++;
			if (c >= 'a' && c <= 'z')
				c = (char)(c - 32);
			dst[n++] = c;
		}
	}
	while (n > 0 && dst[n - 1] == ' ')
		n--;
	dst[n] = 0;
}

void mmb_cmd_help(void)
{
	char topic[48];

	if (at_end())
	{
		mmb_ihelp_open("");
		return;
	}
	if (mmb_match("BASIC"))
	{
		if (at_end())
		{
			mmb_ihelp_open("BASIC");
			return;
		}
	}
	read_topic(topic, sizeof(topic));
	mmb_ihelp_open(topic);
}
