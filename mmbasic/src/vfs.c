#include "mmb_priv.h"

/* Ramdisk node budget shared by the A: ramdisk and the build-time ramdisk/
 * seeding (scripts/gen_ramdisk.py reads this define). Each seeded file and
 * each directory it needs counts as one node. */
#define VFS_MAX 256
#define PKG_MAX 96
#define MMB_DRIVE_LO  'A'
#define MMB_DRIVE_HI  'H'

typedef struct vfs_node {
	char name[80];
	int is_dir;
	unsigned size, cap;
	unsigned char *data;
	int parent;
	int used;
} vfs_node;

typedef struct {
	int letter;
	char path[128]; /* absolute from volume root, starts with '/' */
} mmb_xpath;

static vfs_node nodes[VFS_MAX];
static int ram_cwd;
static vfs_node pkg_nodes[PKG_MAX];
static int pkg_cwd;
static int pkg_on;
static char pkg_prev[128];
static int pkg_have_prev;

/* ---- ramdisk (A: and package B:) ----------------------------------- */

static vfs_node *vol_nodes(int letter, int *max, int **cwd)
{
	if (letter == 'B')
	{
		*max = PKG_MAX;
		if (cwd)
			*cwd = &pkg_cwd;
		return pkg_nodes;
	}
	*max = VFS_MAX;
	if (cwd)
		*cwd = &ram_cwd;
	return nodes;
}

static int ram_find_child(vfs_node *ns, int max, int parent, const char *name)
{
	int i;
	for (i = 0; i < max; i++)
		if (ns[i].used && ns[i].parent == parent &&
		    mmb_keyword_eq(ns[i].name, name))
			return i;
	return -1;
}

static int ram_walk(vfs_node *ns, int max, const char *path, int create_file, int create_dir)
{
	char buf[160], *p, *tok;
	int node = 0;
	if (!path || !path[0] || mmb_keyword_eq(path, "/"))
		return 0;
	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	p = buf;
	if (*p == '/')
		p++;
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
			if (ns[node].parent >= 0)
				node = ns[node].parent;
			continue;
		}
		{
			int ch = ram_find_child(ns, max, node, tok);
			if (ch < 0)
			{
				int i, last = !*p;
				if (!(create_dir || create_file))
					return -1;
				for (i = 0; i < max; i++)
					if (!ns[i].used)
						break;
				if (i >= max)
					return -1;
				memset(&ns[i], 0, sizeof(ns[i]));
				strncpy(ns[i].name, tok, 79);
				ns[i].is_dir = create_dir || !last;
				ns[i].parent = node;
				ns[i].used = 1;
				ch = i;
			}
			node = ch;
		}
	}
	return node;
}

static void ram_path_from_node(vfs_node *ns, int node, char *out, int outsz)
{
	char stack[8][80];
	int sp = 0, n = node;
	out[0] = 0;
	while (n > 0 && sp < 8)
	{
		strncpy(stack[sp++], ns[n].name, 79);
		n = ns[n].parent;
	}
	strncpy(out, "/", (unsigned)outsz - 1);
	out[outsz - 1] = 0;
	while (sp--)
	{
		if (out[1])
			strncat(out, "/", (unsigned)outsz - strlen(out) - 1);
		strncat(out, stack[sp], (unsigned)outsz - strlen(out) - 1);
	}
}

int mmb_glob_match(const char *name, const char *pat)
{
	const char *n, *p, *star, *match;
	if (!pat || !pat[0] || (pat[0] == '*' && pat[1] == 0))
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

static int ram_list(vfs_node *ns, int max, const char *dir, const char *pat, char *out, int outsz)
{
	int parent = ram_walk(ns, max, dir, 0, 0), i, n = 0;
	out[0] = 0;
	if (parent < 0 || !ns[parent].is_dir)
		return -1;
	for (i = 0; i < max; i++)
	{
		int len;
		if (!ns[i].used || ns[i].parent != parent)
			continue;
		if (mmb_vfs_hidden_name(ns[i].name))
			continue;
		if (pat && pat[0] && !mmb_glob_match(ns[i].name, pat))
			continue;
		len = (int)strlen(out);
		if (len + 90 >= outsz)
			break;
		if (n++)
			strcat(out, "\n");
		strcat(out, ns[i].name);
		if (ns[i].is_dir)
			strcat(out, "/");
	}
	return 0;
}

/* Structured ramdisk listing (#621): the node already carries is_dir and the
 * byte size, so no extra lookup is needed. */
static int ram_list_entries(vfs_node *ns, int vmax, const char *dir,
			    const char *pat, mmb_dirent *out, int max, int *truncated)
{
	int parent = ram_walk(ns, vmax, dir, 0, 0), i, n = 0;
	if (parent < 0 || !ns[parent].is_dir)
		return -1;
	for (i = 0; i < vmax; i++)
	{
		if (!ns[i].used || ns[i].parent != parent)
			continue;
		if (mmb_vfs_hidden_name(ns[i].name))
			continue;
		if (pat && pat[0] && !mmb_glob_match(ns[i].name, pat))
			continue;
		if (n >= max)
		{
			if (truncated)
				*truncated = 1;
			break;
		}
		memset(&out[n], 0, sizeof(out[n]));
		strncpy(out[n].name, ns[i].name, sizeof(out[n].name) - 1);
		out[n].is_dir = ns[i].is_dir;
		out[n].size = ns[i].is_dir ? -1 : (int)ns[i].size;
		n++;
	}
	return n;
}

static int ram_write(vfs_node *ns, int max, const char *path, const void *data, unsigned n, int append)
{
	int id = ram_walk(ns, max, path, 1, 0);
	unsigned need;
	if (id < 0 || ns[id].is_dir)
		return -1;
	need = append ? ns[id].size + n : n;
	if (need + 1 > ns[id].cap)
	{
		unsigned cap = need + 64;
		unsigned char *p = G.plat->alloc(cap);
		if (!p)
			return -1;
		if (ns[id].data)
		{
			if (append && ns[id].size)
				memcpy(p, ns[id].data, ns[id].size);
			G.plat->free(ns[id].data);
		}
		ns[id].data = p;
		ns[id].cap = cap;
	}
	if (!append)
		ns[id].size = 0;
	if (n)
		memcpy(ns[id].data + ns[id].size, data, n);
	ns[id].size += n;
	ns[id].data[ns[id].size] = 0;
	return 0;
}

static int ram_read_at(vfs_node *ns, int max, const char *path, unsigned pos, void *data, unsigned n, unsigned *got)
{
	int id = ram_walk(ns, max, path, 0, 0);
	unsigned avail;
	*got = 0;
	if (id < 0 || ns[id].is_dir)
		return -1;
	if (pos >= ns[id].size)
		return 0;
	avail = ns[id].size - pos;
	if (avail > n)
		avail = n;
	if (avail && data)
		memcpy(data, ns[id].data + pos, avail);
	*got = avail;
	return 0;
}

/* ---- path parsing ----------------------------------------------------- */

static int drive_letter(char c)
{
	if (c >= 'a' && c <= 'z')
		c = (char)(c - 32);
	if (c >= MMB_DRIVE_LO && c <= MMB_DRIVE_HI)
		return c;
	return 0;
}

static void slash_norm(char *s)
{
	for (; *s; s++)
		if (*s == '\\')
			*s = '/';
}

static void collapse_path(char *path)
{
	char parts[16][80];
	int n = 0;
	char *p = path;
	if (*p == '/')
		p++;
	while (*p)
	{
		char *tok = p, save;
		while (*p && *p != '/')
			p++;
		save = *p;
		*p = 0;
		if (tok[0] && !mmb_keyword_eq(tok, "."))
		{
			if (mmb_keyword_eq(tok, ".."))
			{
				if (n)
					n--;
			}
			else if (n < 16)
			{
				strncpy(parts[n], tok, 79);
				parts[n][79] = 0;
				n++;
			}
		}
		if (!save)
			break;
		p++;
	}
	path[0] = '/';
	path[1] = 0;
	{
		int i;
		for (i = 0; i < n; i++)
		{
			if (path[1])
				strcat(path, "/");
			strcat(path, parts[i]);
		}
	}
}

static void drive_cwd_path(int letter, char *out, int outsz)
{
	if (letter == 'A' || (letter == 'B' && pkg_on))
	{
		char tmp[128];
		int max, *cwd;
		vfs_node *ns = vol_nodes(letter, &max, &cwd);
		ram_path_from_node(ns, *cwd, tmp, sizeof(tmp));
		strncpy(out, tmp, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
		return;
	}
	{
		const char *p = mmb_fat_cwd(letter);
		strncpy(out, p && p[0] ? p : "/", (unsigned)outsz - 1);
		out[outsz - 1] = 0;
	}
}

static void refresh_public_cwd(void)
{
	char rest[128];
	drive_cwd_path(G.drive, rest, sizeof(rest));
	G.cwd[0] = (char)G.drive;
	G.cwd[1] = ':';
	G.cwd[2] = 0;
	if (rest[0] != '/')
		strcat(G.cwd, "/");
	strcat(G.cwd, rest);
}

static int split_path(const char *in, mmb_xpath *out)
{
	char tmp[160];
	const char *p;
	int letter = G.drive ? G.drive : 'A';
	if (!in)
		in = "";
	strncpy(tmp, in, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	slash_norm(tmp);
	p = tmp;
	if (tmp[0] && tmp[1] == ':')
	{
		int d = drive_letter(tmp[0]);
		if (!d)
			return -1;
		letter = d;
		p = tmp + 2;
	}
	out->letter = letter;
	if (p[0] == '/')
	{
		strncpy(out->path, p, sizeof(out->path) - 1);
		out->path[sizeof(out->path) - 1] = 0;
	}
	else if (p[0] == 0)
	{
		drive_cwd_path(letter, out->path, sizeof(out->path));
	}
	else
	{
		char base[128];
		drive_cwd_path(letter, base, sizeof(base));
		if (!base[0] || (base[0] == '/' && base[1] == 0))
		{
			out->path[0] = '/';
			strncpy(out->path + 1, p, sizeof(out->path) - 2);
			out->path[sizeof(out->path) - 1] = 0;
		}
		else
		{
			strncpy(out->path, base, sizeof(out->path) - 1);
			out->path[sizeof(out->path) - 1] = 0;
			if (out->path[strlen(out->path) - 1] != '/')
				strncat(out->path, "/", sizeof(out->path) - strlen(out->path) - 1);
			strncat(out->path, p, sizeof(out->path) - strlen(out->path) - 1);
		}
	}
	collapse_path(out->path);
	return 0;
}

static void split_dir_glob(const char *path, char *dir, char *glob)
{
	const char *slash = 0, *q = path, *base;
	while (*q)
	{
		if (*q == '/')
			slash = q;
		q++;
	}
	base = slash ? slash + 1 : path;
	if (strchr(base, '*') || strchr(base, '?'))
	{
		strncpy(glob, base, 127);
		glob[127] = 0;
		if (!slash || slash == path)
			strcpy(dir, "/");
		else
		{
			int n = (int)(slash - path);
			if (n > 127)
				n = 127;
			memcpy(dir, path, (unsigned)n);
			dir[n] = 0;
		}
	}
	else
	{
		glob[0] = 0;
		strncpy(dir, path, 127);
		dir[127] = 0;
	}
}

static int physical(int letter)
{
	return letter >= 'C' && letter <= MMB_DRIVE_HI;
}

static int require_drive(int letter)
{
	if (letter == 'A')
		return 0;
	if (letter == 'B')
		return pkg_on ? 0 : -1;
	if (!physical(letter))
		return -1;
	return mmb_fat_ready(letter) ? 0 : -1;
}

/* ---- public API ------------------------------------------------------- */

void mmb_vfs_init(void)
{
	memset(nodes, 0, sizeof(nodes));
	strcpy(nodes[0].name, "/");
	nodes[0].is_dir = 1;
	nodes[0].parent = -1;
	nodes[0].used = 1;
	ram_cwd = 0;
	memset(pkg_nodes, 0, sizeof(pkg_nodes));
	pkg_cwd = 0;
	pkg_on = 0;
	pkg_have_prev = 0;
	pkg_prev[0] = 0;
	G.drive = 'A';
	strcpy(G.cwd, "A:/");
}

const char *mmb_vfs_cwd(void)
{
	refresh_public_cwd();
	return G.cwd;
}

int mmb_vfs_resolve(const char *path, char *out, int outsz)
{
	mmb_xpath x;
	if (split_path(path, &x) != 0)
		return -1;
	if (outsz < 4)
		return -1;
	out[0] = (char)x.letter;
	out[1] = ':';
	strncpy(out + 2, x.path, (unsigned)outsz - 3);
	out[outsz - 1] = 0;
	return 0;
}

int mmb_vfs_chdir(const char *path)
{
	mmb_xpath x;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter == 'A' || x.letter == 'B')
	{
		int max, *cwd, n;
		vfs_node *ns;
		if (x.letter == 'B' && !pkg_on)
			return -1;
		ns = vol_nodes(x.letter, &max, &cwd);
		n = ram_walk(ns, max, x.path, 0, 0);
		if (n < 0 || !ns[n].is_dir)
			return -1;
		*cwd = n;
		G.drive = x.letter;
		refresh_public_cwd();
		return 0;
	}
	if (require_drive(x.letter) != 0)
		return -1;
	if (mmb_fat_chdir(x.letter, x.path) != 0)
		return -1;
	G.drive = x.letter;
	refresh_public_cwd();
	return 0;
}

int mmb_vfs_mkdir(const char *path)
{
	mmb_xpath x;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter == 'B')
		return -1;
	if (x.letter == 'A')
		return ram_walk(nodes, VFS_MAX, x.path, 0, 1) < 0 ? -1 : 0;
	if (require_drive(x.letter) != 0)
		return -1;
	return mmb_fat_mkdir(x.letter, x.path);
}

int mmb_vfs_rmdir(const char *path)
{
	mmb_xpath x;
	int n, i, max;
	vfs_node *ns;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter == 'B')
		return -1;
	if (x.letter != 'A')
	{
		if (require_drive(x.letter) != 0)
			return -1;
		return mmb_fat_rmdir(x.letter, x.path);
	}
	ns = nodes;
	max = VFS_MAX;
	n = ram_walk(ns, max, x.path, 0, 0);
	if (n <= 0 || !ns[n].is_dir)
		return -1;
	for (i = 0; i < max; i++)
		if (ns[i].used && ns[i].parent == n)
			return -1;
	ns[n].used = 0;
	return 0;
}

int mmb_vfs_kill(const char *path)
{
	mmb_xpath x;
	int n, max;
	vfs_node *ns;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter == 'B')
		return -1;
	if (x.letter != 'A')
	{
		if (require_drive(x.letter) != 0)
			return -1;
		return mmb_fat_unlink(x.letter, x.path);
	}
	ns = nodes;
	max = VFS_MAX;
	n = ram_walk(ns, max, x.path, 0, 0);
	if (n <= 0 || ns[n].is_dir)
		return -1;
	if (ns[n].data)
		G.plat->free(ns[n].data);
	ns[n].used = 0;
	return 0;
}

int mmb_vfs_exists(const char *path)
{
	mmb_xpath x;
	int max;
	vfs_node *ns;
	if (split_path(path, &x) != 0)
		return 0;
	if (x.letter == 'A' || (x.letter == 'B' && pkg_on))
	{
		ns = vol_nodes(x.letter, &max, 0);
		return ram_walk(ns, max, x.path, 0, 0) >= 0;
	}
	if (require_drive(x.letter) != 0)
		return 0;
	return mmb_fat_exists(x.letter, x.path);
}

int mmb_vfs_size(const char *path)
{
	mmb_xpath x;
	int n, max;
	vfs_node *ns;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter == 'A' || (x.letter == 'B' && pkg_on))
	{
		ns = vol_nodes(x.letter, &max, 0);
		n = ram_walk(ns, max, x.path, 0, 0);
		if (n < 0 || ns[n].is_dir)
			return -1;
		return (int)ns[n].size;
	}
	if (require_drive(x.letter) != 0)
		return -1;
	return mmb_fat_size(x.letter, x.path);
}

int mmb_vfs_write(const char *path, const void *data, unsigned n, int append)
{
	mmb_xpath x;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter == 'B')
		return -1;
	if (x.letter == 'A')
		return ram_write(nodes, VFS_MAX, x.path, data, n, append);
	if (require_drive(x.letter) != 0)
		return -1;
	return mmb_fat_write(x.letter, x.path, data, n, append);
}

/* ---- streaming writes ------------------------------------------------- */

typedef struct {
	int used;
	int is_ram;
	int letter;
	int node;		/* ramdisk node id */
	void *fat;		/* open FIL (console/storage.cpp) */
} vfs_writer;

#define VFS_MAX_WRITERS 2
static vfs_writer writers[VFS_MAX_WRITERS];

/* Like ram_write(), but grows the backing buffer geometrically instead of by
 * ~one chunk per call, so a many-chunk upload is O(n) rather than O(n^2). */
static int ram_stream_write(vfs_node *ns, int node, const void *data, unsigned n)
{
	unsigned need = ns[node].size + n;
	if (need + 1 > ns[node].cap)
	{
		unsigned cap = ns[node].cap ? ns[node].cap : 64;
		unsigned char *p;
		while (cap < need + 1)
			cap *= 2;
		p = G.plat->alloc(cap);
		if (!p)
			return -1;
		if (ns[node].size && ns[node].data)
			memcpy(p, ns[node].data, ns[node].size);
		if (ns[node].data)
			G.plat->free(ns[node].data);
		ns[node].data = p;
		ns[node].cap = cap;
	}
	if (n)
		memcpy(ns[node].data + ns[node].size, data, n);
	ns[node].size += n;
	ns[node].data[ns[node].size] = 0;
	return 0;
}

int mmb_vfs_wopen(const char *path, int append)
{
	mmb_xpath x;
	int i;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter == 'B')
		return -1;
	for (i = 0; i < VFS_MAX_WRITERS; i++)
		if (!writers[i].used)
			break;
	if (i >= VFS_MAX_WRITERS)
		return -1;
	memset(&writers[i], 0, sizeof(writers[i]));
	if (x.letter == 'A')
	{
		int node = ram_walk(nodes, VFS_MAX, x.path, 1, 0);
		if (node < 0 || nodes[node].is_dir)
			return -1;
		if (!append)
		{
			nodes[node].size = 0;
			if (nodes[node].data)
				nodes[node].data[0] = 0;
		}
		writers[i].is_ram = 1;
		writers[i].node = node;
	}
	else
	{
		void *fp;
		if (require_drive(x.letter) != 0)
			return -1;
		fp = mmb_fat_wopen(x.letter, x.path, append);
		if (!fp)
			return -1;
		writers[i].fat = fp;
	}
	writers[i].letter = x.letter;
	writers[i].used = 1;
	return i;
}

int mmb_vfs_wwrite(int handle, const void *data, unsigned n)
{
	if (handle < 0 || handle >= VFS_MAX_WRITERS || !writers[handle].used)
		return -1;
	if (!n)
		return 0;
	if (writers[handle].is_ram)
		return ram_stream_write(nodes, writers[handle].node, data, n);
	return mmb_fat_wwrite(writers[handle].fat, data, n);
}

int mmb_vfs_wclose(int handle)
{
	int rc = 0;
	if (handle < 0 || handle >= VFS_MAX_WRITERS || !writers[handle].used)
		return -1;
	writers[handle].used = 0;
	if (!writers[handle].is_ram)
	{
		rc = mmb_fat_wclose(writers[handle].fat);
		writers[handle].fat = 0;
	}
	return rc;
}

int mmb_vfs_read_at(const char *path, unsigned pos, void *data, unsigned n, unsigned *got)
{
	mmb_xpath x;
	int max;
	vfs_node *ns;
	*got = 0;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter == 'A' || (x.letter == 'B' && pkg_on))
	{
		ns = vol_nodes(x.letter, &max, 0);
		return ram_read_at(ns, max, x.path, pos, data, n, got);
	}
	if (require_drive(x.letter) != 0)
		return -1;
	return mmb_fat_read_at(x.letter, x.path, pos, data, n, got);
}

int mmb_vfs_read(const char *path, void *data, unsigned maxn, unsigned *n)
{
	return mmb_vfs_read_at(path, 0, data, maxn, n);
}

int mmb_vfs_read_ptr(const char *path, const unsigned char **ptr, unsigned *n)
{
	mmb_xpath x;
	int id, max;
	vfs_node *ns;
	*ptr = 0;
	*n = 0;
	if (split_path(path, &x) != 0)
		return -1;
	if (x.letter != 'A' && !(x.letter == 'B' && pkg_on))
		return -1;
	ns = vol_nodes(x.letter, &max, 0);
	id = ram_walk(ns, max, x.path, 0, 0);
	if (id < 0 || ns[id].is_dir)
		return -1;
	*ptr = ns[id].data;
	*n = ns[id].size;
	return 0;
}

int mmb_vfs_copy(const char *src, const char *dst)
{
	int sz, off;
	unsigned char buf[1024];
	sz = mmb_vfs_size(src);
	if (sz < 0)
		return -1;
	if (mmb_vfs_write(dst, "", 0, 0) != 0)
		return -1;
	for (off = 0; off < sz; )
	{
		unsigned got = 0;
		unsigned chunk = (unsigned)sz - (unsigned)off;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);
		if (mmb_vfs_read_at(src, (unsigned)off, buf, chunk, &got) != 0)
			return -1;
		if (!got)
			break;
		if (mmb_vfs_write(dst, buf, got, 1) != 0)
			return -1;
		off += (int)got;
	}
	return 0;
}

static const char *last_slash(const char *path)
{
	const char *s = 0;
	while (*path)
	{
		if (*path == '/')
			s = path;
		path++;
	}
	return s;
}

int mmb_vfs_rename(const char *src, const char *dst)
{
	mmb_xpath a, b;
	int s, ch;
	char dir[80], base[80];
	const char *slash;
	if (split_path(src, &a) != 0 || split_path(dst, &b) != 0)
		return -1;
	if (a.letter != b.letter)
		return -1;
	if (a.letter == 'B')
		return -1;
	if (a.letter != 'A')
	{
		if (require_drive(a.letter) != 0)
			return -1;
		return mmb_fat_rename(a.letter, a.path, b.path);
	}
	s = ram_walk(nodes, VFS_MAX, a.path, 0, 0);
	if (s < 0)
		return -1;
	slash = last_slash(b.path);
	if (!slash || !slash[1])
		return -1;
	{
		int n = (int)(slash - b.path);
		if (n == 0)
		{
			dir[0] = '/';
			dir[1] = 0;
		}
		else
		{
			if (n > 79)
				n = 79;
			memcpy(dir, b.path, (unsigned)n);
			dir[n] = 0;
		}
		strncpy(base, slash + 1, 79);
		base[79] = 0;
	}
	if (ram_walk(nodes, VFS_MAX, dir, 0, 0) != nodes[s].parent)
		return -1;
	ch = ram_find_child(nodes, VFS_MAX, nodes[s].parent, base);
	if (ch >= 0 && ch != s)
		return -1;
	strncpy(nodes[s].name, base, 79);
	nodes[s].name[79] = 0;
	return 0;
}

int mmb_vfs_hidden_name(const char *name)
{
	return name && name[0] == '.';
}

#define VFS_LIST_MAX 512

static int list_cmp_names(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	int ad = la > 0 && a[la - 1] == '/';
	int bd = lb > 0 && b[lb - 1] == '/';
	int i;
	if (ad != bd)
		return bd - ad; /* folders first */
	for (i = 0; a[i] && b[i]; i++)
	{
		int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
		if (ca >= 'a' && ca <= 'z')
			ca -= 32;
		if (cb >= 'a' && cb <= 'z')
			cb -= 32;
		if (ca != cb)
			return ca - cb;
	}
	{
		int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
		if (ca >= 'a' && ca <= 'z')
			ca -= 32;
		if (cb >= 'a' && cb <= 'z')
			cb -= 32;
		return ca - cb;
	}
}

/* Sort a newline-separated listing in place: folders A-Z first, then files
 * A-Z, case-insensitively. Shell sort keeps this O(n log n)-ish for the large
 * folders that used to bog down in the old insertion sort (#621). */
static void sort_dir_list(char *out, int outsz)
{
	char *ptrs[VFS_LIST_MAX];
	char *tmp;
	int n = 0, i, gap, len = 0;
	char *p = out;
	while (*p && n < VFS_LIST_MAX)
	{
		ptrs[n++] = p;
		while (*p && *p != '\n')
			p++;
		if (*p == '\n')
			*p++ = 0;
	}
	if (n < 2)
		return;
	for (gap = n / 2; gap > 0; gap /= 2)
		for (i = gap; i < n; i++)
		{
			char *key = ptrs[i];
			int j = i;
			while (j >= gap && list_cmp_names(ptrs[j - gap], key) > 0)
			{
				ptrs[j] = ptrs[j - gap];
				j -= gap;
			}
			ptrs[j] = key;
		}
	tmp = G.plat ? G.plat->alloc((unsigned)outsz) : 0;
	if (!tmp)
	{
		for (i = 0; i + 1 < n; i++)
			ptrs[i][strlen(ptrs[i])] = '\n';
		return;
	}
	for (i = 0; i < n; i++)
	{
		int l = (int)strlen(ptrs[i]);
		if (len + l + 2 > outsz)
			break;
		memcpy(tmp + len, ptrs[i], (unsigned)l);
		len += l;
		if (i + 1 < n)
			tmp[len++] = '\n';
	}
	tmp[len] = 0;
	memcpy(out, tmp, (unsigned)len + 1);
	G.plat->free(tmp);
}

/* Comparator for the structured listing: folders first, then case-insensitive
 * by name. Names carry no trailing slash there, so list_cmp_names' folder test
 * is a no-op and only its ordering matters. */
static int dirent_cmp(const mmb_dirent *a, const mmb_dirent *b)
{
	if (a->is_dir != b->is_dir)
		return a->is_dir ? -1 : 1;
	return list_cmp_names(a->name, b->name);
}

static void sort_dir_entries(mmb_dirent *e, int n)
{
	int i, gap;
	for (gap = n / 2; gap > 0; gap /= 2)
		for (i = gap; i < n; i++)
		{
			mmb_dirent key = e[i];
			int j = i;
			while (j >= gap && dirent_cmp(&e[j - gap], &key) > 0)
			{
				e[j] = e[j - gap];
				j -= gap;
			}
			e[j] = key;
		}
}

int mmb_vfs_list(const char *spec, char *out, int outsz)
{
	mmb_xpath x;
	char dir[128], glob[128];
	out[0] = 0;
	if (split_path(spec ? spec : "", &x) != 0)
		return -1;
	split_dir_glob(x.path, dir, glob);
	if (x.letter == 'A' || (x.letter == 'B' && pkg_on))
	{
		int max;
		vfs_node *ns = vol_nodes(x.letter, &max, 0);
		if (ram_list(ns, max, dir, glob, out, outsz) != 0)
			return -1;
		sort_dir_list(out, outsz);
		return 0;
	}
	if (require_drive(x.letter) != 0)
		return -1;
	if (mmb_fat_list(x.letter, dir, glob, out, outsz) != 0)
		return -1;
	sort_dir_list(out, outsz);
	return 0;
}

int mmb_vfs_list_entries(const char *spec, mmb_dirent *out, int max,
			 int *truncated)
{
	mmb_xpath x;
	char dir[128], glob[128];
	int n;

	if (truncated)
		*truncated = 0;
	if (!out || max <= 0)
		return -1;
	if (split_path(spec ? spec : "", &x) != 0)
		return -1;
	split_dir_glob(x.path, dir, glob);
	if (x.letter == 'A' || (x.letter == 'B' && pkg_on))
	{
		int vmax;
		vfs_node *ns = vol_nodes(x.letter, &vmax, 0);
		n = ram_list_entries(ns, vmax, dir, glob, out, max, truncated);
	}
	else
	{
		if (require_drive(x.letter) != 0)
			return -1;
		n = mmb_fat_list_entries(x.letter, dir, glob, out, max, truncated);
	}
	if (n < 0)
		return -1;
	sort_dir_entries(out, n);
	return n;
}

void mmb_vfs_seed_file(const char *path, const void *data, unsigned n)
{
	mmb_vfs_write(path, data, n, 0);
}

void mmb_vfs_drives(char *out, int outsz)
{
	int i;
	out[0] = 0;
	strncat(out, "A: RAM", (unsigned)outsz - 1);
	for (i = 'C'; i <= MMB_DRIVE_HI; i++)
	{
		char line[80];
		line[0] = 0;
		mmb_fat_drive_line(i, line, sizeof(line));
		if (!line[0])
			continue;
		if ((int)strlen(out) + (int)strlen(line) + 2 >= outsz)
			break;
		strcat(out, "\n");
		strcat(out, line);
	}
}

int mmb_vfs_isdir(const char *path)
{
	mmb_xpath x;
	int n, max;
	vfs_node *ns;
	if (split_path(path, &x) != 0)
		return 0;
	if (x.letter == 'A' || (x.letter == 'B' && pkg_on))
	{
		ns = vol_nodes(x.letter, &max, 0);
		n = ram_walk(ns, max, x.path, 0, 0);
		return n >= 0 && ns[n].is_dir;
	}
	if (require_drive(x.letter) != 0)
		return 0;
	return mmb_fat_isdir(x.letter, x.path);
}

int mmb_vfs_readonly_path(const char *path)
{
	mmb_xpath x;
	if (split_path(path, &x) != 0)
		return 0;
	return x.letter == 'B';
}

int mmb_pkg_mounted(void)
{
	return pkg_on;
}

static void pkg_free_nodes(void)
{
	int i;
	for (i = 0; i < PKG_MAX; i++)
	{
		if (pkg_nodes[i].used && pkg_nodes[i].data)
			G.plat->free(pkg_nodes[i].data);
		pkg_nodes[i].used = 0;
		pkg_nodes[i].data = 0;
	}
	memset(pkg_nodes, 0, sizeof(pkg_nodes));
	strcpy(pkg_nodes[0].name, "/");
	pkg_nodes[0].is_dir = 1;
	pkg_nodes[0].parent = -1;
	pkg_nodes[0].used = 1;
	pkg_cwd = 0;
}

void mmb_pkg_unmount(void)
{
	char saved[128];
	int have, i;
	have = pkg_have_prev;
	strncpy(saved, pkg_prev, sizeof(saved) - 1);
	saved[sizeof(saved) - 1] = 0;
	pkg_have_prev = 0;
	pkg_prev[0] = 0;
	for (i = 1; i <= MMB_MAX_FILES; i++)
	{
		if (G.files[i].open && G.files[i].path[0] &&
		    (G.files[i].path[0] == 'B' || G.files[i].path[0] == 'b') &&
		    G.files[i].path[1] == ':')
			G.files[i].open = 0;
	}
	pkg_free_nodes();
	pkg_on = 0;
	if (G.drive == 'B')
	{
		G.drive = 'A';
		refresh_public_cwd();
	}
	if (have && saved[0])
		mmb_vfs_chdir(saved);
}

static int pkg_add_file(const char *path, const void *data, unsigned n, void *ctx)
{
	int *have_main = (int *)ctx;
	char full[160];
	int nested = 0;
	const char *q, *base;
	full[0] = '/';
	strncpy(full + 1, path, sizeof(full) - 2);
	full[sizeof(full) - 1] = 0;
	if (n == 0 && !data)
		return ram_walk(pkg_nodes, PKG_MAX, full, 0, 1) < 0 ? -1 : 0;
	{
		char dir[160];
		char *p;
		strncpy(dir, full, sizeof(dir) - 1);
		dir[sizeof(dir) - 1] = 0;
		p = dir + 1;
		while (*p)
		{
			while (*p && *p != '/')
				p++;
			if (*p == 0)
				break;
			*p = 0;
			if (ram_walk(pkg_nodes, PKG_MAX, dir, 0, 1) < 0)
				return -1;
			*p = '/';
			p++;
		}
	}
	if (ram_write(pkg_nodes, PKG_MAX, full, data, n, 0) != 0)
		return -1;
	base = path;
	for (q = path; *q; q++)
	{
		if (*q == '/')
		{
			nested = 1;
			base = q + 1;
		}
	}
	if (!nested && mmb_keyword_eq(base, "MAIN.BAS"))
		*have_main = 1;
	return 0;
}

int mmb_pkg_is_name(const char *path)
{
	const char *dot = 0, *p = path ? path : "";
	while (*p)
	{
		if (*p == '.' )
			dot = p;
		if (*p == '/' || *p == '\\')
			dot = 0;
		p++;
	}
	return dot && mmb_keyword_eq(dot, ".APP");
}

int mmb_pkg_mount(const char *path)
{
	unsigned char *buf = 0;
	unsigned got = 0;
	int sz, have_main = 0;
	char saved[128];

	mmb_pkg_unmount();
	sz = mmb_vfs_size(path);
	if (sz < 0)
		mmb_error("?FILE NOT FOUND");
	if ((unsigned)sz > MMB_ZIP_MAX_BYTES)
		mmb_error("?PACKAGE");
	buf = G.plat->alloc((unsigned)sz + 1);
	if (!buf)
		mmb_error("?OUT OF MEMORY");
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) != 0)
	{
		G.plat->free(buf);
		mmb_error("?FILE NOT FOUND");
	}
	strncpy(saved, mmb_vfs_cwd(), sizeof(saved) - 1);
	saved[sizeof(saved) - 1] = 0;
	pkg_free_nodes();
	if (mmb_zip_foreach(buf, got, pkg_add_file, &have_main) != 0)
	{
		pkg_free_nodes();
		G.plat->free(buf);
		mmb_error("?PACKAGE");
	}
	G.plat->free(buf);
	if (!have_main)
	{
		pkg_free_nodes();
		mmb_error("?NO MAIN.BAS");
	}
	strncpy(pkg_prev, saved, sizeof(pkg_prev) - 1);
	pkg_prev[sizeof(pkg_prev) - 1] = 0;
	pkg_have_prev = 1;
	pkg_on = 1;
	if (mmb_vfs_chdir("B:/") != 0)
	{
		mmb_pkg_unmount();
		mmb_error("?PACKAGE");
	}
	return 0;
}

void mmb_cmd_drive(void)
{
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		char spec[8];
		if (v.type != T_STR)
			mmb_syntax();
		strncpy(spec, v.s, sizeof(spec) - 1);
		spec[sizeof(spec) - 1] = 0;
		if (!strchr(spec, ':'))
			strncat(spec, ":", sizeof(spec) - strlen(spec) - 1);
		if (!G.running && spec[0] && (spec[0] == 'B' || spec[0] == 'b') &&
		    spec[1] == ':')
			mmb_error("?DRIVE");
		if (mmb_vfs_chdir(spec) != 0)
			mmb_error("?DRIVE");
		return;
	}
	{
		char buf[512];
		mmb_vfs_drives(buf, sizeof(buf));
		mmb_out(buf);
	}
}

/* EJECT ["D:"] — flush and unmount a removable volume so the stick can be
 * pulled without corrupting its FAT. Defaults to the current drive. The
 * system SD/ramdisk are never ejectable. */
void mmb_cmd_eject(void)
{
	int letter;
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		if (v.type != T_STR || !v.s[0])
			mmb_syntax();
		letter = (unsigned char)v.s[0];
		if (letter >= 'a' && letter <= 'z')
			letter = letter - 32;
	}
	else
		letter = G.drive;
	if (letter == 'A' || letter == 'B')
		mmb_error("?EJECT");
	if (mmb_fat_eject(letter) != 0)
		mmb_error("?EJECT");
	if (G.drive == letter)
	{
		G.drive = 'A';
		refresh_public_cwd();
	}
}

/* Storage hotplug notices. A full-screen app can claim the message (FILES
 * puts it in the hint line); otherwise print it between prompt lines. */
void mmb_storage_notice(const char *msg)
{
	char line[160];
	const char *p;
	int n = 0;
	if (!msg || !msg[0])
		return;
	if (mmb_files_notice(msg))
		return;
	line[n++] = '\r';
	line[n++] = '\n';
	for (p = msg; *p && n < (int)sizeof(line) - 3; p++)
		line[n++] = *p;
	line[n++] = '\r';
	line[n++] = '\n';
	line[n] = 0;
	mmb_console_write(line);
}
