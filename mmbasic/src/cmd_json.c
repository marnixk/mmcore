#include "mmb_priv.h"
#include "cJSON.h"

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

static void json_item_text(const cJSON *item, char *out, int outsz)
{
	out[0] = 0;
	if (!item || outsz < 2)
		return;
	if (cJSON_IsNull(item) || cJSON_IsObject(item) || cJSON_IsArray(item) ||
	    cJSON_IsInvalid(item))
		return;
	if (cJSON_IsBool(item))
	{
		strncpy(out, cJSON_IsTrue(item) ? "true" : "false", (unsigned)outsz - 1);
		out[outsz - 1] = 0;
		return;
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
		G.outn = save;
		G.out[G.outn] = 0;
		return;
	}
	if (cJSON_IsString(item) && item->valuestring)
	{
		strncpy(out, item->valuestring, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
	}
}

mmb_val mmb_json_query(const char *js, const char *path)
{
	cJSON *parse;
	const cJSON *item;
	char buf[MMB_MAX_STR + 1];

	json_hooks();
	buf[0] = 0;
	if (!js)
		js = "";
	if (!path)
		path = "";
	parse = cJSON_Parse(js);
	if (!parse)
		mmb_error("?JSON");
	item = json_walk(parse, path);
	json_item_text(item, buf, sizeof(buf));
	cJSON_Delete(parse);
	return mmb_str_val(buf);
}
