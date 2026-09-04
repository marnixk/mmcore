#include "storage.h"
#include <circle/util.h>

/*
 * FatFs volumes (circle/addon/fatfs):
 *   0 SD:    emmc1  → C:  (SD card slot, always this letter)
 *   1 SD2:   emmc2
 *   2 USB:   umsd1  → D:
 *   3 USB2:  umsd2  → E:
 *   4 USB3:  umsd3  → F:
 *   5 FD:    ufd1   → G:
 *   6 NVME:  nvme1  → H:
 */

static const struct {
	char letter;
	const char *vol;	/* "SD:" */
	const char *kind;	/* shown by DRIVE */
	int always;		/* list even if empty (SD slot) */
} kMap[] = {
	{ 'C', "SD:",   "SD",   1 },
	{ 'D', "USB:",  "USB",  0 },
	{ 'E', "USB2:", "USB",  0 },
	{ 'F', "USB3:", "USB",  0 },
	{ 'G', "FD:",   "USB",  0 },
	{ 'H', "NVME:", "NVME", 0 },
};

static const int kNMap = (int)(sizeof kMap / sizeof kMap[0]);

static CStorage *s_st;
static char s_cwd[8][128];
static int s_ready[8];

static int map_index(int letter)
{
	int i;
	for (i = 0; i < kNMap; i++)
		if (kMap[i].letter == letter)
			return i;
	return -1;
}

static int slot_of(int letter)
{
	int i = map_index(letter);
	return i < 0 ? -1 : i;
}

static void make_full(int letter, const char *path, char *out, unsigned outsz)
{
	int i = map_index(letter);
	const char *vol = i >= 0 ? kMap[i].vol : "SD:";
	unsigned n = 0;
	while (vol[n] && n + 1 < outsz)
	{
		out[n] = vol[n];
		n++;
	}
	if (!path || !path[0])
		path = "/";
	while (*path && n + 1 < outsz)
		out[n++] = *path++;
	out[n] = 0;
}

static void try_mount(int idx)
{
	if (idx < 0 || idx >= kNMap)
		return;
	if (s_ready[idx])
		return;
	if (!s_st)
		return;
	if (f_mount(&s_st->m_fs[idx], kMap[idx].vol, 1) == FR_OK)
	{
		s_ready[idx] = 1;
		if (!s_cwd[idx][0])
			strcpy(s_cwd[idx], "/");
	}
}

CStorage::CStorage (CInterruptSystem *irq, CTimer *timer, CActLED *led)
:	m_EMMC (irq, timer, led),
	m_USBHCI (irq, timer, TRUE)
{
}

CStorage::~CStorage (void)
{
}

boolean CStorage::Initialize (void)
{
	int i;
	s_st = this;
	memset(s_cwd, 0, sizeof s_cwd);
	memset(s_ready, 0, sizeof s_ready);
	for (i = 0; i < kNMap; i++)
		strcpy(s_cwd[i], "/");

	m_EMMC.Initialize ();
	/* USB host may have no devices; never fail the kernel for that. */
	m_USBHCI.Initialize ();

	try_mount(map_index('C'));
	try_mount(map_index('D'));
	try_mount(map_index('E'));
	try_mount(map_index('F'));
	try_mount(map_index('G'));
	try_mount(map_index('H'));
	return TRUE;
}

void CStorage::Poll (void)
{
	m_USBHCI.UpdatePlugAndPlay ();
	try_mount(map_index('C'));
	try_mount(map_index('D'));
	try_mount(map_index('E'));
	try_mount(map_index('F'));
	try_mount(map_index('G'));
	try_mount(map_index('H'));
}

void mmb_storage_bind (CStorage *st)
{
	s_st = st;
}

extern "C" {

void mmb_storage_poll (void)
{
	if (s_st)
		s_st->Poll ();
}

int mmb_fat_ready(int letter)
{
	int i = slot_of(letter);
	return i >= 0 && s_ready[i];
}

const char *mmb_fat_cwd(int letter)
{
	int i = slot_of(letter);
	if (i < 0)
		return "/";
	return s_cwd[i][0] ? s_cwd[i] : "/";
}

void mmb_fat_drive_line(int letter, char *out, int outsz)
{
	int i = map_index(letter);
	out[0] = 0;
	if (i < 0)
		return;
	if (!kMap[i].always && !s_ready[i])
		return;
	/* "C: SD" or "C: SD (no media)" */
	{
		char buf[40];
		unsigned n = 0;
		buf[n++] = kMap[i].letter;
		buf[n++] = ':';
		buf[n++] = ' ';
		{
			const char *k = kMap[i].kind;
			while (*k && n < sizeof(buf) - 1)
				buf[n++] = *k++;
		}
		if (!s_ready[i])
		{
			const char *k = " (no media)";
			while (*k && n < sizeof(buf) - 1)
				buf[n++] = *k++;
		}
		buf[n] = 0;
		strncpy(out, buf, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
	}
}

int mmb_fat_chdir(int letter, const char *path)
{
	FILINFO inf;
	char full[160];
	int i = slot_of(letter);
	if (i < 0 || !s_ready[i])
		return -1;
	if (!path || !path[0] || (path[0] == '/' && path[1] == 0))
	{
		strcpy(s_cwd[i], "/");
		return 0;
	}
	make_full(letter, path, full, sizeof full);
	if (f_stat(full, &inf) != FR_OK || !(inf.fattrib & AM_DIR))
		return -1;
	strncpy(s_cwd[i], path, sizeof(s_cwd[i]) - 1);
	s_cwd[i][sizeof(s_cwd[i]) - 1] = 0;
	return 0;
}

static int mkdir_parents(int letter, const char *path)
{
	char tmp[160], full[160];
	unsigned i;
	if (!path || path[0] != '/')
		return -1;
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	for (i = 1; tmp[i]; i++)
	{
		if (tmp[i] != '/')
			continue;
		tmp[i] = 0;
		make_full(letter, tmp, full, sizeof full);
		f_mkdir(full);
		tmp[i] = '/';
	}
	make_full(letter, tmp, full, sizeof full);
	if (f_mkdir(full) != FR_OK)
	{
		FILINFO inf;
		if (f_stat(full, &inf) != FR_OK)
			return -1;
	}
	return 0;
}

int mmb_fat_mkdir(int letter, const char *path)
{
	if (!mmb_fat_ready(letter))
		return -1;
	return mkdir_parents(letter, path);
}

int mmb_fat_rmdir(int letter, const char *path)
{
	char full[160];
	if (!mmb_fat_ready(letter))
		return -1;
	make_full(letter, path, full, sizeof full);
	return f_rmdir(full) == FR_OK ? 0 : -1;
}

int mmb_fat_unlink(int letter, const char *path)
{
	char full[160];
	if (!mmb_fat_ready(letter))
		return -1;
	make_full(letter, path, full, sizeof full);
	return f_unlink(full) == FR_OK ? 0 : -1;
}

int mmb_fat_rename(int letter, const char *from, const char *to)
{
	char a[160], b[160];
	if (!mmb_fat_ready(letter))
		return -1;
	make_full(letter, from, a, sizeof a);
	make_full(letter, to, b, sizeof b);
	return f_rename(a, b) == FR_OK ? 0 : -1;
}

int mmb_fat_exists(int letter, const char *path)
{
	FILINFO inf;
	char full[160];
	if (!mmb_fat_ready(letter))
		return 0;
	make_full(letter, path, full, sizeof full);
	return f_stat(full, &inf) == FR_OK;
}

int mmb_fat_size(int letter, const char *path)
{
	FILINFO inf;
	char full[160];
	if (!mmb_fat_ready(letter))
		return -1;
	make_full(letter, path, full, sizeof full);
	if (f_stat(full, &inf) != FR_OK)
		return -1;
	if (inf.fattrib & AM_DIR)
		return -1;
	return (int)inf.fsize;
}

int mmb_fat_write(int letter, const char *path, const void *data, unsigned n, int append)
{
	FIL fp;
	char full[160];
	BYTE mode;
	UINT bw = 0;
	if (!mmb_fat_ready(letter))
		return -1;
	make_full(letter, path, full, sizeof full);
	mode = append ? (BYTE)(FA_OPEN_ALWAYS | FA_WRITE) : (BYTE)(FA_CREATE_ALWAYS | FA_WRITE);
	if (f_open(&fp, full, mode) != FR_OK)
		return -1;
	if (append)
		f_lseek(&fp, f_size(&fp));
	if (n && f_write(&fp, data, n, &bw) != FR_OK)
	{
		f_close(&fp);
		return -1;
	}
	f_close(&fp);
	return 0;
}

int mmb_fat_read_at(int letter, const char *path, unsigned pos, void *data, unsigned n, unsigned *got)
{
	FIL fp;
	char full[160];
	UINT br = 0;
	*got = 0;
	if (!mmb_fat_ready(letter))
		return -1;
	make_full(letter, path, full, sizeof full);
	if (f_open(&fp, full, FA_READ) != FR_OK)
		return -1;
	if (pos)
		f_lseek(&fp, pos);
	if (n && f_read(&fp, data, n, &br) != FR_OK)
	{
		f_close(&fp);
		return -1;
	}
	f_close(&fp);
	*got = br;
	return 0;
}

int mmb_fat_list(int letter, const char *dir, const char *pat, char *out, int outsz)
{
	DIR dp;
	FILINFO inf;
	char full[160];
	int nent = 0;
	out[0] = 0;
	if (!mmb_fat_ready(letter))
		return -1;
	make_full(letter, dir, full, sizeof full);
	if (f_opendir(&dp, full) != FR_OK)
		return -1;
	for (;;)
	{
		if (f_readdir(&dp, &inf) != FR_OK || inf.fname[0] == 0)
			break;
		if (inf.fname[0] == '.')
			continue;
		if (pat && pat[0] && pat[0] != '*')
		{
			/* simple: let FatFs-style match fall back to listing all when pat is "*" */
		}
		if (pat && pat[0])
		{
			/* reuse MMBasic glob via case-insensitive compare in vfs — duplicate tiny match */
			const char *name = inf.fname;
			const char *p = pat;
			const char *star = 0, *match = 0;
			int ok = 1;
			if (!(p[0] == '*' && p[1] == 0))
			{
				while (*name)
				{
					char cn = *name, cp = *p;
					if (cn >= 'a' && cn <= 'z') cn = (char)(cn - 32);
					if (cp >= 'a' && cp <= 'z') cp = (char)(cp - 32);
					if (cp == '*') { star = p++; match = name; continue; }
					if (cp == '?' || cn == cp) { name++; p++; continue; }
					if (star) { p = star + 1; match++; name = match; continue; }
					ok = 0; break;
				}
				if (ok)
				{
					while (*p == '*') p++;
					if (*p) ok = 0;
				}
			}
			if (!ok)
				continue;
		}
		{
			int len = (int)strlen(out);
			int need = (int)strlen(inf.fname) + 2;
			if (len + need >= outsz)
				break;
			if (nent++)
				strcat(out, "\n");
			strcat(out, inf.fname);
			if (inf.fattrib & AM_DIR)
				strcat(out, "/");
		}
	}
	f_closedir(&dp);
	return 0;
}

}
