#include "mmb_priv.h"
#include <string.h>

/* PicoMite-compatible ARRAY SET / ADD / INSERT / SLICE for plain arrays. */

static mmb_arrview parse_whole_array(void)
{
	mmb_arrview a;
	mmb_skip_sp();
	if (!mmb_try_parse_arrview(&a))
		mmb_syntax();
	if (a.moff >= 0 || a.v->type == T_STRUCT)
		mmb_error("?UNSUPPORTED");
	return a;
}

static void store_str(mmb_arrview a, int i, const char *s)
{
	mmb_str_set(&a.v->data.s[i], s ? s : "", -1, a.v->maxlen, a.v->name);
}

static void array_set(void)
{
	mmb_val val = mmb_expr();
	mmb_arrview dst;
	int i;
	mmb_skip_sp();
	mmb_expect(',');
	dst = parse_whole_array();
	if (val.type == T_STR)
	{
		if (dst.v->type != T_STR)
			mmb_error("?TYPE MISMATCH");
		for (i = 0; i < dst.count; i++)
			store_str(dst, i, val.s);
	}
	else
	{
		if (dst.v->type == T_STR)
			mmb_error("?TYPE MISMATCH");
		for (i = 0; i < dst.count; i++)
			mmb_arrview_set(dst, i, mmb_as_float(val));
	}
}

static void array_add(void)
{
	mmb_arrview src, dst;
	mmb_val val;
	int i;
	src = parse_whole_array();
	mmb_skip_sp();
	mmb_expect(',');
	val = mmb_expr();
	mmb_skip_sp();
	mmb_expect(',');
	dst = parse_whole_array();
	if (src.count != dst.count)
		mmb_error("?SIZE MISMATCH");
	if (src.v->type == T_STR || dst.v->type == T_STR)
	{
		if (src.v->type != T_STR || dst.v->type != T_STR || val.type != T_STR)
			mmb_error("?TYPE MISMATCH");
		for (i = 0; i < src.count; i++)
		{
			const char *a0 = src.v->data.s[i];
			int la = a0 ? (int)strlen(a0) : 0;
			int lb = val.s ? (int)strlen(val.s) : 0;
			char *buf = mmb_tmp_alloc(la + lb + 1);
			if (la)
				memcpy(buf, a0, (size_t)la);
			if (lb)
				memcpy(buf + la, val.s, (size_t)lb);
			buf[la + lb] = 0;
			store_str(dst, i, buf);
		}
	}
	else
	{
		if (val.type == T_STR)
			mmb_error("?TYPE MISMATCH");
		for (i = 0; i < src.count; i++)
			mmb_arrview_set(dst, i, mmb_arrview_get(src, i) + mmb_as_float(val));
	}
}

/* Parse index1, index2, ... (one may be omitted / empty) before a trailing
   whole-array reference. */
static int parse_slice_indices(mmb_var *v, int *pos, int *omit)
{
	int base = G.opt.base;
	int len[MMB_MAX_DIMS], i;
	*omit = -1;
	for (i = 0; i < v->dims; i++)
		len[i] = v->dim[i] - base + 1;
	for (i = 0; i < v->dims; i++)
	{
		mmb_skip_sp();
		if (*G.p == ',')
			pos[i] = -1;
		else
		{
			mmb_val pv = mmb_expr();
			pos[i] = (int)mmb_as_int(pv);
			if (pos[i] < base || pos[i] > v->dim[i])
				mmb_error("?INDEX OUT OF BOUNDS");
			mmb_skip_sp();
		}
		if (*G.p != ',')
			mmb_syntax();
		G.p++;
		if (pos[i] < 0)
		{
			if (*omit >= 0)
				mmb_error("?SLICE");
			*omit = i;
		}
	}
	return 0;
}

static void array_slice_common(int insert)
{
	mmb_arrview first, second;
	mmb_var *mv;
	int pos[MMB_MAX_DIMS], omit = -1, stride[MMB_MAX_DIMS];
	int i, len[MMB_MAX_DIMS], base_off = 0, inc, slice_len, k, base = G.opt.base;

	first = parse_whole_array();
	mv = first.v;
	if (mv->dims < 2)
		mmb_error("?ARRAY");
	mmb_skip_sp();
	mmb_expect(',');
	parse_slice_indices(mv, pos, &omit);
	if (omit < 0)
		mmb_error("?SLICE");
	mmb_skip_sp();
	second = parse_whole_array();

	if (!insert)
	{
		/* first = source (>=2D), second = 1D destination */
		if (second.v->dims != 1)
			mmb_error("?ARRAY");
	}
	else
	{
		/* first = destination (>=2D), second = 1D source */
		if (second.v->dims != 1)
			mmb_error("?ARRAY");
	}
	for (i = 0; i < mv->dims; i++)
		len[i] = mv->dim[i] - base + 1;
	stride[mv->dims - 1] = 1;
	for (i = mv->dims - 2; i >= 0; i--)
		stride[i] = stride[i + 1] * len[i + 1];
	for (i = 0; i < mv->dims; i++)
		if (i != omit)
			base_off += (pos[i] - base) * stride[i];
	inc = stride[omit];
	slice_len = len[omit];
	if (second.count != slice_len)
		mmb_error("?SIZE MISMATCH");
	if ((mv->type == T_STR) != (second.v->type == T_STR))
		mmb_error("?TYPE MISMATCH");

	if (mv->type == T_STR)
	{
		for (k = 0; k < slice_len; k++)
		{
			int moff = base_off + k * inc;
			if (insert)
				store_str(first, moff, second.v->data.s[k]);
			else
				store_str(second, k, first.v->data.s[moff]);
		}
	}
	else
	{
		for (k = 0; k < slice_len; k++)
		{
			int moff = base_off + k * inc;
			if (insert)
				mmb_arrview_set(first, moff, mmb_arrview_get(second, k));
			else
				mmb_arrview_set(second, k, mmb_arrview_get(first, moff));
		}
	}
}

void mmb_cmd_array(void)
{
	mmb_skip_sp();
	if (mmb_match("SET"))
	{
		array_set();
		return;
	}
	if (mmb_match("ADD"))
	{
		array_add();
		return;
	}
	if (mmb_match("INSERT"))
	{
		array_slice_common(1);
		return;
	}
	if (mmb_match("SLICE"))
	{
		array_slice_common(0);
		return;
	}
	mmb_syntax();
}
