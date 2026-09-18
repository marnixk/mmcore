#include "mmb_priv.h"
#include "cJSON.h"
#include <stdio.h>

static void *json_malloc(size_t n)
{
	if (!G.plat || !G.plat->alloc)
		return 0;
	return G.plat->alloc((unsigned)n);
}

static void json_free(void *p)
{
	if (p && G.plat && G.plat->free)
		G.plat->free(p);
}

static void json_hooks(void)
{
	static int done;
	cJSON_Hooks h;

	if (done)
		return;
	h.malloc_fn = json_malloc;
	h.free_fn = json_free;
	cJSON_InitHooks(&h);
	done = 1;
}

static const cJSON *json_walk(const cJSON *cur, const char *path)
{
	char key[40];
	int ki = 0;

	if (!cur)
		return 0;
	while (*path)
	{
		if (*path == '.')
		{
			if (ki)
			{
				key[ki] = 0;
				cur = cJSON_GetObjectItem(cur, key);
				ki = 0;
				if (!cur)
					return 0;
			}
			path++;
			continue;
		}
		if (*path == '[')
		{
			int idx = 0, neg = 0;

			if (ki)
			{
				key[ki] = 0;
				cur = cJSON_GetObjectItem(cur, key);
				ki = 0;
				if (!cur)
					return 0;
			}
			path++;
			if (*path == '-')
			{
				neg = 1;
				path++;
			}
			if (*path < '0' || *path > '9')
				return 0;
			while (*path >= '0' && *path <= '9')
			{
				idx = idx * 10 + (*path - '0');
				path++;
			}
			if (*path != ']')
				return 0;
			path++;
			if (neg)
				idx = -idx;
			cur = cJSON_GetArrayItem(cur, idx);
			if (!cur)
				return 0;
			continue;
		}
		if (ki + 1 < (int)sizeof(key))
			key[ki++] = *path;
		path++;
	}
	if (ki)
	{
		key[ki] = 0;
		cur = cJSON_GetObjectItem(cur, key);
	}
	return cur;
}

static int json_item_text(const cJSON *item, char *out, int outsz)
{
	int need = 0;
	out[0] = 0;
	if (!item || outsz < 2)
		return 0;
	if (cJSON_IsNull(item) || cJSON_IsObject(item) || cJSON_IsArray(item) ||
	    cJSON_IsInvalid(item))
		return 0;
	if (cJSON_IsBool(item))
	{
		const char *s = cJSON_IsTrue(item) ? "true" : "false";
		need = (int)strlen(s);
		strncpy(out, s, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
		return need;
	}
	if (cJSON_IsNumber(item))
	{
		int save = G.outn;
		mmb_val v;

		G.outn = 0;
		G.out[0] = 0;
		if ((double)(int64_t)item->valuedouble == item->valuedouble)
			v = mmb_int_val((int64_t)item->valuedouble);
		else
			v = mmb_num_val(item->valuedouble);
		mmb_print_val(v);
		strncpy(out, G.out, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
		need = G.outn;
		G.outn = save;
		G.out[G.outn] = 0;
		return need;
	}
	if (cJSON_IsString(item) && item->valuestring)
	{
		need = (int)strlen(item->valuestring);
		strncpy(out, item->valuestring, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
		return need;
	}
	return 0;
}

mmb_val mmb_json_query(const char *js, const char *path)
{
	cJSON *parse;
	const cJSON *item;
	int cap = 256;
	char *buf;

	json_hooks();
	if (!js)
		js = "";
	if (!path)
		path = "";
	parse = cJSON_Parse(js);
	if (!parse)
		mmb_error("?JSON");
	item = json_walk(parse, path);
	for (;;)
	{
		int need;
		buf = mmb_tmp_alloc(cap);
		need = json_item_text(item, buf, cap);
		if (need < cap)
			break;
		cap = need + 1;
	}
	cJSON_Delete(parse);
	return mmb_str_val(buf);
}

static int mem_esz(const mmb_smem *m)
{
	if (m->type == T_INT || m->type == T_NUM)
		return 8;
	if (m->type == T_STR)
		return m->size + MMB_STRUCT_STRLEN;
	if (m->type == T_STRUCT && m->size >= 0 && m->size < G.nstruct)
		return G.sdef[m->size].total;
	return 0;
}

static void json_pack_str(unsigned char *p, int max, const char *s)
{
	int n = 0;

	if (!s)
		s = "";
	while (s[n] && n < max)
		n++;
	MMB_STRUCT_STRLEN_PUT(p, n);
	if (n)
		memcpy(p + MMB_STRUCT_STRLEN, s, (unsigned)n);
	if (n < max)
		memset(p + MMB_STRUCT_STRLEN + n, 0, (unsigned)(max - n));
}

static void json_unpack_str(const unsigned char *p, int max, char *out, int outsz)
{
	int n = MMB_STRUCT_STRLEN_GET(p);

	if (n < 0)
		n = 0;
	if (n > max)
		n = max;
	if (n > outsz - 1)
		n = outsz - 1;
	if (n)
		memcpy(out, p + MMB_STRUCT_STRLEN, (unsigned)n);
	out[n] = 0;
}

static int64_t json_to_int(const cJSON *item)
{
	double x = item->valuedouble;

	if (x != x || x >= 9223372036854775808.0 || x <= -9223372036854775808.0)
		mmb_error("?OVERFLOW");
	return (int64_t)x;
}

static void json_fill_value(unsigned char *p, const mmb_smem *m, const cJSON *item);
static void json_fill_struct(unsigned char *rec, int sid, const cJSON *obj);

static void json_fill_value(unsigned char *p, const mmb_smem *m, const cJSON *item)
{
	if (!item || cJSON_IsNull(item) || cJSON_IsInvalid(item))
		return;
	if (m->type == T_INT)
	{
		int64_t x;

		if (cJSON_IsBool(item))
			x = cJSON_IsTrue(item) ? 1 : 0;
		else if (cJSON_IsNumber(item))
			x = json_to_int(item);
		else
			return;
		memcpy(p, &x, 8);
		return;
	}
	if (m->type == T_NUM)
	{
		double x;

		if (cJSON_IsBool(item))
			x = cJSON_IsTrue(item) ? 1.0 : 0.0;
		else if (cJSON_IsNumber(item))
			x = item->valuedouble;
		else
			return;
		memcpy(p, &x, 8);
		return;
	}
	if (m->type == T_STR)
	{
		if (cJSON_IsString(item) && item->valuestring)
		{
			int slen = (int)strlen(item->valuestring);
			if (slen > m->size)
			{
				char msg[160];
				sprintf(msg, "?OVERFLOW: %s needs %d, LENGTH %d",
					m->name, slen, m->size);
				mmb_error(msg);
			}
			json_pack_str(p, m->size, item->valuestring);
		}
		return;
	}
	if (m->type == T_STRUCT && cJSON_IsObject(item))
		json_fill_struct(p, m->size, item);
}

static void json_fill_struct(unsigned char *rec, int sid, const cJSON *obj)
{
	mmb_sdef *d;
	int i;

	if (!rec || sid < 0 || sid >= G.nstruct || !cJSON_IsObject(obj))
		return;
	d = &G.sdef[sid];
	for (i = 0; i < d->nmem; i++)
	{
		mmb_smem *m = &d->mem[i];
		const cJSON *item = cJSON_GetObjectItem(obj, m->name);
		int esz, n, j;

		if (!item || cJSON_IsNull(item))
			continue;
		esz = mem_esz(m);
		if (esz <= 0)
			continue;
		if (m->dims > 0)
		{
			if (!cJSON_IsArray(item))
				continue;
			n = cJSON_GetArraySize(item);
			if (n > m->count)
				n = m->count;
			for (j = 0; j < n; j++)
				json_fill_value(rec + m->offset + j * esz, m,
						cJSON_GetArrayItem(item, j));
		}
		else
			json_fill_value(rec + m->offset, m, item);
	}
}

void mmb_cmd_json_parse(void)
{
	mmb_val js;
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t, eoff;
	mmb_var *v;
	cJSON *root;

	js = mmb_expr();
	if (js.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	mmb_skip_sp();
	mmb_expect(',');
	mmb_skip_sp();
	G.acc_on = 0;
	t = mmb_parse_var_ref(name, &nidx, idx);
	v = mmb_find_var(name, t, 0, nidx, idx);
	if (!v || v->type != T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	if (G.acc_on)
		mmb_error("?TYPE MISMATCH");
	eoff = nidx ? mmb_var_offset(v, nidx, idx) : 0;
	json_hooks();
	root = cJSON_Parse(js.s ? js.s : "");
	if (!root)
		mmb_error("?JSON");
	if (v->dims > 0 && nidx == 0)
	{
		if (cJSON_IsArray(root))
		{
			int n = cJSON_GetArraySize(root);
			int i;

			if (n > v->size)
				n = v->size;
			for (i = 0; i < n; i++)
				json_fill_struct(mmb_struct_elem(v, i), v->struct_idx,
						 cJSON_GetArrayItem(root, i));
		}
	}
	else
		json_fill_struct(mmb_struct_elem(v, eoff), v->struct_idx, root);
	cJSON_Delete(root);
}

static int emit_ch(char *dst, int cap, int n, char c)
{
	if (n < 0 || n + 1 >= cap)
		return -1;
	dst[n] = c;
	return n + 1;
}

static int emit_str(char *dst, int cap, int n, const char *s)
{
	if (n < 0)
		return -1;
	while (*s)
	{
		n = emit_ch(dst, cap, n, *s++);
		if (n < 0)
			return -1;
	}
	return n;
}

static int emit_quoted(char *dst, int cap, int n, const char *s)
{
	n = emit_ch(dst, cap, n, '"');
	if (n < 0)
		return -1;
	if (!s)
		s = "";
	while (*s)
	{
		unsigned char c = (unsigned char)*s++;
		char hex[8];
		const char *esc = 0;

		if (c == '"' || c == '\\')
		{
			n = emit_ch(dst, cap, n, '\\');
			if (n < 0)
				return -1;
			n = emit_ch(dst, cap, n, (char)c);
		}
		else if (c == '\n')
			esc = "\\n";
		else if (c == '\r')
			esc = "\\r";
		else if (c == '\t')
			esc = "\\t";
		else if (c < 32)
		{
			sprintf(hex, "\\u%04x", c);
			n = emit_str(dst, cap, n, hex);
		}
		else
			n = emit_ch(dst, cap, n, (char)c);
		if (esc)
			n = emit_str(dst, cap, n, esc);
		if (n < 0)
			return -1;
	}
	return emit_ch(dst, cap, n, '"');
}

static int emit_i64(char *dst, int cap, int n, int64_t v)
{
	char tmp[24];
	int t = 0;
	uint64_t u;

	if (v < 0)
	{
		n = emit_ch(dst, cap, n, '-');
		if (n < 0)
			return -1;
		u = (uint64_t)(-(v + 1)) + 1;
	}
	else
		u = (uint64_t)v;
	if (u == 0)
		tmp[t++] = '0';
	while (u)
	{
		tmp[t++] = (char)('0' + (u % 10));
		u /= 10;
	}
	while (t)
	{
		n = emit_ch(dst, cap, n, tmp[--t]);
		if (n < 0)
			return -1;
	}
	return n;
}

static int emit_num(char *dst, int cap, int n, double x)
{
	char tmp[48];

	sprintf(tmp, "%1.15g", x);
	return emit_str(dst, cap, n, tmp);
}

static int emit_struct(char *dst, int cap, int n, const unsigned char *rec, int sid);

static int emit_value(char *dst, int cap, int n, const unsigned char *p, const mmb_smem *m)
{
	if (m->type == T_INT)
	{
		int64_t x = 0;
		memcpy(&x, p, 8);
		return emit_i64(dst, cap, n, x);
	}
	if (m->type == T_NUM)
	{
		double x = 0;
		memcpy(&x, p, 8);
		return emit_num(dst, cap, n, x);
	}
	if (m->type == T_STR)
	{
		char *tmp = mmb_tmp_alloc(m->size + 1);
		json_unpack_str(p, m->size, tmp, m->size + 1);
		return emit_quoted(dst, cap, n, tmp);
	}
	if (m->type == T_STRUCT)
		return emit_struct(dst, cap, n, p, m->size);
	return n;
}

static int emit_struct(char *dst, int cap, int n, const unsigned char *rec, int sid)
{
	mmb_sdef *d;
	int i, first = 1;

	if (!rec || sid < 0 || sid >= G.nstruct)
		return n;
	d = &G.sdef[sid];
	n = emit_ch(dst, cap, n, '{');
	if (n < 0)
		return -1;
	for (i = 0; i < d->nmem; i++)
	{
		mmb_smem *m = &d->mem[i];
		int esz = mem_esz(m);
		int j;

		if (esz <= 0)
			continue;
		if (!first)
		{
			n = emit_ch(dst, cap, n, ',');
			if (n < 0)
				return -1;
		}
		first = 0;
		n = emit_quoted(dst, cap, n, m->name);
		if (n < 0)
			return -1;
		n = emit_ch(dst, cap, n, ':');
		if (n < 0)
			return -1;
		if (m->dims > 0)
		{
			n = emit_ch(dst, cap, n, '[');
			if (n < 0)
				return -1;
			for (j = 0; j < m->count; j++)
			{
				if (j)
				{
					n = emit_ch(dst, cap, n, ',');
					if (n < 0)
						return -1;
				}
				n = emit_value(dst, cap, n, rec + m->offset + j * esz, m);
				if (n < 0)
					return -1;
			}
			n = emit_ch(dst, cap, n, ']');
		}
		else
			n = emit_value(dst, cap, n, rec + m->offset, m);
		if (n < 0)
			return -1;
	}
	return emit_ch(dst, cap, n, '}');
}

mmb_val mmb_json_stringify(mmb_val v)
{
	int cap = 256;
	char *buf;
	int n;

	if (v.type != T_STRUCT || !v.blob)
		mmb_error("?TYPE MISMATCH");
	for (;;)
	{
		buf = mmb_tmp_alloc(cap);
		n = emit_struct(buf, cap, 0, v.blob, v.struct_idx);
		if (n >= 0)
			break;
		if (cap > (1 << 24))
			mmb_error("?OVERFLOW");
		cap *= 2;
	}
	buf[n] = 0;
	return mmb_arena_val(buf);
}
