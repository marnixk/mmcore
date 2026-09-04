#include "mmb_priv.h"

#define VFS_MAX 128

typedef struct vfs_node {
	char name[80];
	int is_dir;
	unsigned size, cap;
	unsigned char *data;
	int parent;
	int used;
} vfs_node;

static vfs_node nodes[VFS_MAX];
static int cwd;
static const char *list_pattern;

void mmb_vfs_list_set_pattern(const char *pat)
{
	list_pattern = pat;
}

static void split_path(const char *path, char *dir, char *base)
{
	const char *slash = 0, *p = path;
	while (*p)
	{
		if (*p == '/')
			slash = p;
		p++;
	}
	if (!slash)
	{
		dir[0] = 0;
		strncpy(base, path, 79);
		base[79] = 0;
		return;
	}
	{
		int n = (int)(slash - path);
		if (n >= 79)
			n = 79;
		memcpy(dir, path, (unsigned)n);
		dir[n] = 0;
		strncpy(base, slash + 1, 79);
		base[79] = 0;
	}
}

static int find_child(int parent, const char *name)
{
	int i;
	for (i = 0; i < VFS_MAX; i++)
		if (nodes[i].used && nodes[i].parent == parent &&
		    mmb_keyword_eq(nodes[i].name, name))
			return i;
	return -1;
}

static int walk(const char *path, int create_file, int create_dir)
{
	char buf[160], *p, *tok;
	int node = (*path == '/') ? 0 : cwd;
	if (!path || !path[0] || mmb_keyword_eq(path, "."))
		return node;
	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	p = buf;
	if (*p == '/')
	{
		node = 0;
		p++;
	}
	while (*p)
	{
		tok = p;
		while (*p && *p != '/')
			p++;
		if (*p == '/')
			*p++ = 0;
		if (tok[0] == 0 || mmb_keyword_eq(tok, "."))
			continue;
		if (mmb_keyword_eq(tok, ".."))
		{
			if (nodes[node].parent >= 0)
				node = nodes[node].parent;
			continue;
		}
		{
			int ch = find_child(node, tok);
			if (ch < 0)
			{
				int i, last = !*p;
				if (!(create_dir || (create_file && last)))
					return -1;
				for (i = 0; i < VFS_MAX; i++)
					if (!nodes[i].used)
						break;
				if (i >= VFS_MAX)
					return -1;
				memset(&nodes[i], 0, sizeof(nodes[i]));
				strncpy(nodes[i].name, tok, 79);
				nodes[i].is_dir = create_dir || !last;
				nodes[i].parent = node;
				nodes[i].used = 1;
				ch = i;
			}
			node = ch;
		}
	}
	return node;
}

void mmb_vfs_init(void)
{
	memset(nodes, 0, sizeof(nodes));
	strcpy(nodes[0].name, "/");
	nodes[0].is_dir = 1;
	nodes[0].parent = -1;
	nodes[0].used = 1;
	cwd = 0;
	strcpy(G.cwd, "/");
}

const char *mmb_vfs_cwd(void)
{
	return G.cwd;
}

static void rebuild_cwd(void)
{
	char stack[8][80];
	int sp = 0, n = cwd;
	G.cwd[0] = 0;
	while (n > 0 && sp < 8)
	{
		strncpy(stack[sp++], nodes[n].name, 79);
		n = nodes[n].parent;
	}
	strcpy(G.cwd, "/");
	while (sp--)
	{
		if (G.cwd[1])
			strcat(G.cwd, "/");
		strcat(G.cwd, stack[sp]);
	}
}

int mmb_vfs_chdir(const char *path)
{
	int n = walk(path, 0, 0);
	if (n < 0 || !nodes[n].is_dir)
		return -1;
	cwd = n;
	rebuild_cwd();
	return 0;
}

int mmb_vfs_mkdir(const char *path)
{
	int n = walk(path, 0, 1);
	return n < 0 ? -1 : 0;
}

int mmb_vfs_rmdir(const char *path)
{
	int n = walk(path, 0, 0), i;
	if (n <= 0 || !nodes[n].is_dir)
		return -1;
	for (i = 0; i < VFS_MAX; i++)
		if (nodes[i].used && nodes[i].parent == n)
			return -1;
	nodes[n].used = 0;
	return 0;
}

int mmb_vfs_kill(const char *path)
{
	int n = walk(path, 0, 0);
	if (n <= 0 || nodes[n].is_dir)
		return -1;
	if (nodes[n].data)
		G.plat->free(nodes[n].data);
	nodes[n].used = 0;
	return 0;
}

int mmb_vfs_exists(const char *path)
{
	return walk(path, 0, 0) >= 0;
}

int mmb_vfs_size(const char *path)
{
	int n = walk(path, 0, 0);
	if (n < 0 || nodes[n].is_dir)
		return -1;
	return (int)nodes[n].size;
}

int mmb_vfs_write(const char *path, const void *data, unsigned n, int append)
{
	int id = walk(path, 1, 0);
	unsigned need;
	if (id < 0 || nodes[id].is_dir)
		return -1;
	need = append ? nodes[id].size + n : n;
	if (need + 1 > nodes[id].cap)
	{
		unsigned cap = need + 64;
		unsigned char *p = G.plat->alloc(cap);
		if (!p)
			return -1;
		if (nodes[id].data)
		{
			if (append && nodes[id].size)
				memcpy(p, nodes[id].data, nodes[id].size);
			G.plat->free(nodes[id].data);
		}
		nodes[id].data = p;
		nodes[id].cap = cap;
	}
	if (!append)
		nodes[id].size = 0;
	memcpy(nodes[id].data + nodes[id].size, data, n);
	nodes[id].size += n;
	nodes[id].data[nodes[id].size] = 0;
	return 0;
}

int mmb_vfs_read(const char *path, void *data, unsigned maxn, unsigned *got)
{
	int id = walk(path, 0, 0);
	unsigned n;
	*got = 0;
	if (id < 0 || nodes[id].is_dir)
		return -1;
	n = nodes[id].size;
	if (n > maxn)
		n = maxn;
	if (n && data)
		memcpy(data, nodes[id].data, n);
	*got = n;
	return 0;
}

int mmb_vfs_copy(const char *src, const char *dst)
{
	int s = walk(src, 0, 0);
	if (s < 0 || nodes[s].is_dir)
		return -1;
	return mmb_vfs_write(dst, nodes[s].data, nodes[s].size, 0);
}

static int name_has_path(const char *name)
{
	const char *p = name;
	while (*p)
	{
		if (*p == '/' || *p == '\\')
			return 1;
		p++;
	}
	return 0;
}

static int glob_match(const char *name, const char *pat)
{
	const char *n, *p, *star, *match;
	if (!pat || !pat[0])
		return 1;
	n = name;
	p = pat;
	star = 0;
	match = 0;
	while (*n)
	{
		char cn = *n, cp = *p;
		if (cn >= 'a' && cn <= 'z')
			cn = (char)(cn - 32);
		if (cp >= 'a' && cp <= 'z')
			cp = (char)(cp - 32);
		if (cp == '*')
		{
			star = p++;
			match = n;
			continue;
		}
		if (cp == '?' || cn == cp)
		{
			n++;
			p++;
			continue;
		}
		if (star)
		{
			p = star + 1;
			match++;
			n = match;
			continue;
		}
		return 0;
	}
	while (*p == '*')
		p++;
	return *p == 0;
}

int mmb_vfs_rename(const char *src, const char *dst)
{
	int s = walk(src, 0, 0);
	char dir[80], base[80];
	int ch;
	if (s < 0 || nodes[s].parent != cwd)
		return -1;
	if (name_has_path(dst))
		return -1;
	split_path(dst, dir, base);
	if (dir[0] || !base[0])
		return -1;
	ch = find_child(cwd, base);
	if (ch >= 0 && ch != s)
		return -1;
	strncpy(nodes[s].name, base, 79);
	nodes[s].name[79] = 0;
	return 0;
}

int mmb_vfs_list(char *out, int outsz)
{
	int i, n = 0;
	out[0] = 0;
	for (i = 0; i < VFS_MAX; i++)
	{
		if (nodes[i].used && nodes[i].parent == cwd)
		{
			int len;
			if (list_pattern && !glob_match(nodes[i].name, list_pattern))
				continue;
			len = (int)strlen(out);
			if (len + 90 >= outsz)
				break;
			if (n++)
				strcat(out, "\n");
			strcat(out, nodes[i].name);
			if (nodes[i].is_dir)
				strcat(out, "/");
		}
	}
	return 0;
}

void mmb_vfs_seed_file(const char *path, const void *data, unsigned n)
{
	mmb_vfs_write(path, data, n, 0);
}

int mmb_vfs_read_ptr(const char *path, const unsigned char **ptr, unsigned *n)
{
	int id = walk(path, 0, 0);
	if (id < 0 || nodes[id].is_dir)
		return -1;
	*ptr = nodes[id].data;
	*n = nodes[id].size;
	return 0;
}
