#include "storage.h"
#include "mmbasic.h"
#include <circle/util.h>
#include <fatfs/diskio.h>
#include <stdlib.h>

/*
 * FatFs volumes (circle/addon/fatfs):
 *   0 SD:    emmc1  → C:  (SD card slot, always this letter)
 *   2 USB:   umsd1  → D:
 *   3 USB2:  umsd2  → E:
 *   4 USB3:  umsd3  → F:
 *   5 FD:    ufd1   → G:
 *   6 NVME:  nvme1  → H:
 *
 * pd is the FatFs physical drive (LD2PD(vol) == vol); SD2: is skipped, so the
 * USB/NVME letters map one higher than their kMap index.
 */

extern "C" void mmb_storage_notice (const char *msg);

static const struct {
	char letter;
	const char *vol;	/* "SD:" */
	const char *kind;	/* shown by DRIVE */
	int always;		/* list even if empty (SD slot) */
	int pd;			/* FatFs physical drive number */
} kMap[] = {
	{ 'C', "SD:",   "SD",   1, 0 },
	{ 'D', "USB:",  "USB",  0, 2 },
	{ 'E', "USB2:", "USB",  0, 3 },
	{ 'F', "USB3:", "USB",  0, 4 },
	{ 'G', "FD:",   "USB",  0, 5 },
	{ 'H', "NVME:", "NVME", 0, 6 },
};

static const int kNMap = (int)(sizeof kMap / sizeof kMap[0]);

static CStorage *s_st;
static int s_ready[8];
static int s_ejected[8];	/* user ejected; wait for a physical re-insert */
static int s_present[8];	/* underlying disk reported the device present */
static int s_notify_ready;	/* suppress notices during initial probe */

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

/* USB sticks and other removable media get hotplug notices. */
static int is_removable(int idx)
{
	return kMap[idx].kind[0] == 'U';
}

static void notice_mount(int idx, const char *verb)
{
	char msg[48];
	unsigned n = 0;
	if (!s_notify_ready || !is_removable(idx))
		return;
	msg[n++] = kMap[idx].letter;
	msg[n++] = ':';
	msg[n++] = ' ';
	{
		const char *k = kMap[idx].kind;
		while (*k && n < sizeof(msg) - 16)
			msg[n++] = *k++;
	}
	msg[n++] = ' ';
	{
		const char *v = verb;
		while (*v && n < sizeof(msg) - 1)
			msg[n++] = *v++;
	}
	msg[n] = 0;
	mmb_storage_notice(msg);
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
	if (s_ready[idx] || s_ejected[idx])
		return;
	if (!s_st)
		return;
	if (f_mount(&s_st->m_fs[idx], kMap[idx].vol, 1) == FR_OK)
	{
		s_ready[idx] = 1;
		s_present[idx] = 1;
		notice_mount(idx, "mounted");
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
	s_st = this;
	memset(s_ready, 0, sizeof s_ready);
	memset(s_ejected, 0, sizeof s_ejected);
	memset(s_present, 0, sizeof s_present);
	s_notify_ready = 0;

	m_EMMC.Initialize ();
	/* USB host may have no devices; never fail the kernel for that. */
	m_USBHCI.Initialize ();

	try_mount(map_index('C'));
	try_mount(map_index('D'));
	try_mount(map_index('E'));
	try_mount(map_index('F'));
	try_mount(map_index('G'));
	try_mount(map_index('H'));
	s_notify_ready = 1;
	return TRUE;
}

void CStorage::Poll (void)
{
	int i;
	m_USBHCI.UpdatePlugAndPlay ();
	for (i = 0; i < kNMap; i++)
	{
		int present = disk_status((BYTE)kMap[i].pd) == 0;
		if (s_ready[i] && !present)
		{
			/* Stick pulled without ejecting: drop the stale mount. */
			f_mount(0, kMap[i].vol, 0);
			s_ready[i] = 0;
			s_ejected[i] = 0;
			s_present[i] = 0;
			notice_mount(i, "removed");
		}
		else if (!present)
		{
			s_present[i] = 0;
			/* Gone: clear a stale eject so the next insert mounts. */
			s_ejected[i] = 0;
		}
		else
			s_present[i] = 1;
	}
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

void mmb_storage_unmount (void)
{
	int i;
	if (!s_st)
		return;
	for (i = 0; i < kNMap; i++)
	{
		if (!s_ready[i])
			continue;
		f_mount (0, kMap[i].vol, 0);
		s_ready[i] = 0;
	}
	memset(s_ejected, 0, sizeof s_ejected);
	memset(s_present, 0, sizeof s_present);
	s_notify_ready = 0;
}

int mmb_fat_ready(int letter)
{
	int i = slot_of(letter);
	return i >= 0 && s_ready[i];
}

/* Read the FAT volume label from the boot sector. A superfloppy has the VBR
 * in sector 0; a partitioned stick has an MBR first, so follow the first FAT
 * partition. Returns 0 (with out[] empty) when there is no usable label. */
static int read_volume_label(int idx, char *out, int outsz)
{
	BYTE sec[512];
	BYTE vbr[512];
	BYTE pdrv = (BYTE)kMap[idx].pd;
	int off, i, n = 0;
	out[0] = 0;
	if (disk_read(pdrv, sec, 0, 1) != RES_OK)
		return -1;
	if (!(sec[0] == 0xEB || sec[0] == 0xE9))
	{
		int j, found = 0;
		UINT lba = 0;
		for (j = 0; j < 4; j++)
		{
			BYTE *e = sec + 0x1BE + j * 16;
			BYTE type = e[4];
			if (type == 0x01 || type == 0x04 || type == 0x06 ||
			    type == 0x0B || type == 0x0C || type == 0x0E)
			{
				lba = (UINT)e[8] | ((UINT)e[9] << 8) |
				      ((UINT)e[10] << 16) | ((UINT)e[11] << 24);
				found = 1;
				break;
			}
		}
		if (!found || disk_read(pdrv, vbr, lba, 1) != RES_OK)
			return -1;
	}
	else
		memcpy(vbr, sec, sizeof vbr);
	if (vbr[510] != 0x55 || vbr[511] != 0xAA)
		return -1;
	off = (vbr[0x52] == 'F' && vbr[0x53] == 'A' && vbr[0x54] == 'T' &&
	       vbr[0x55] == '3' && vbr[0x56] == '2') ? 0x47 : 0x2B;
	for (i = 0; i < 11 && n < outsz - 1; i++)
		out[n++] = (char)vbr[off + i];
	out[n] = 0;
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = 0;
	if (n == 0 || strcmp(out, "NO NAME") == 0)
		out[0] = 0;
	return 0;
}

int mmb_fat_label(int letter, char *out, int outsz)
{
	int i = slot_of(letter);
	if (out && outsz > 0)
		out[0] = 0;
	if (i < 0 || !s_ready[i] || !out || outsz <= 0)
		return -1;
	return read_volume_label(i, out, outsz);
}

/* Flush and unmount a removable volume so it can be pulled safely. */
int mmb_fat_eject(int letter)
{
	int i = slot_of(letter);
	char msg[48];
	unsigned n = 0;
	if (i < 0 || letter == 'C' || !s_ready[i])
		return -1;
	disk_ioctl((BYTE)kMap[i].pd, CTRL_SYNC, 0);
	f_mount (0, kMap[i].vol, 0);
	s_ready[i] = 0;
	s_ejected[i] = 1;
	msg[n++] = kMap[i].letter;
	msg[n++] = ':';
	msg[n++] = ' ';
	{
		const char *k = kMap[i].kind;
		while (*k && n < sizeof(msg) - 10)
			msg[n++] = *k++;
	}
	{
		const char *v = " ejected";
		while (*v && n < sizeof(msg) - 1)
			msg[n++] = *v++;
	}
	msg[n] = 0;
	if (s_notify_ready)
		mmb_storage_notice(msg);
	return 0;
}

void mmb_fat_drive_line(int letter, char *out, int outsz)
{
	int i = map_index(letter);
	out[0] = 0;
	if (i < 0)
		return;
	if (!kMap[i].always && !s_ready[i])
		return;
	/* "C: SD" / "C: SD \"MMBASIC\"" / "C: SD (no media)" */
	{
		char buf[64];
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
		else
		{
			char lab[16];
			if (mmb_fat_label(letter, lab, sizeof lab) == 0 && lab[0])
			{
				const char *p = lab;
				if (n < sizeof(buf) - 3)
				{
					buf[n++] = ' ';
					buf[n++] = '"';
				}
				while (*p && n < sizeof(buf) - 2)
					buf[n++] = *p++;
				if (n < sizeof(buf) - 1)
					buf[n++] = '"';
			}
		}
		buf[n] = 0;
		strncpy(out, buf, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
	}
}

/* Validate that a directory-absolute path names a directory. The current
 * directory is session state and lives in the per-console mmb; only the drive
 * mount table is global. */
int mmb_fat_chdir(int letter, const char *path)
{
	FILINFO inf;
	char full[160];
	int i = slot_of(letter);
	if (i < 0 || !s_ready[i])
		return -1;
	if (!path || !path[0] || (path[0] == '/' && path[1] == 0))
		return 0;
	make_full(letter, path, full, sizeof full);
	if (f_stat(full, &inf) != FR_OK || !(inf.fattrib & AM_DIR))
		return -1;
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

int mmb_fat_isdir(int letter, const char *path)
{
	FILINFO inf;
	char full[160];
	if (!mmb_fat_ready(letter))
		return 0;
	/* FatFs f_stat() rejects the volume root (NS_NONAME -> FR_INVALID_NAME),
	 * but the root of a mounted volume is always a directory. */
	if (!path || !path[0] || (path[0] == '/' && path[1] == 0))
		return 1;
	make_full(letter, path, full, sizeof full);
	return f_stat(full, &inf) == FR_OK && (inf.fattrib & AM_DIR);
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

void *mmb_fat_wopen(int letter, const char *path, int append)
{
	FIL *fp;
	char full[160];
	BYTE mode;
	if (!mmb_fat_ready(letter))
		return 0;
	fp = (FIL *) malloc(sizeof(FIL));
	if (!fp)
		return 0;
	make_full(letter, path, full, sizeof full);
	mode = append ? (BYTE)(FA_OPEN_ALWAYS | FA_WRITE) : (BYTE)(FA_CREATE_ALWAYS | FA_WRITE);
	if (f_open(fp, full, mode) != FR_OK)
	{
		free(fp);
		return 0;
	}
	if (append)
		f_lseek(fp, f_size(fp));
	return fp;
}

int mmb_fat_wwrite(void *handle, const void *data, unsigned n)
{
	FIL *fp = (FIL *)handle;
	UINT bw = 0;
	if (!fp)
		return -1;
	if (n && f_write(fp, data, n, &bw) != FR_OK)
		return -1;
	if (bw != n)
		return -1;		/* short write (e.g. volume full) */
	return 0;
}

int mmb_fat_wclose(void *handle)
{
	FIL *fp = (FIL *)handle;
	FRESULT r;
	if (!fp)
		return -1;
	r = f_close(fp);
	free(fp);
	return r == FR_OK ? 0 : -1;
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

/* Case-insensitive glob used by the FAT directory listings: `*` and `?`,
 * matching MMBasic's own mmb_glob_match. */
static int fat_pat_match(const char *name, const char *pat)
{
	const char *star = 0, *match = 0;
	while (*name)
	{
		char cn = *name, cp = *pat;
		if (cp == '*')
		{
			star = pat++;
			match = name;
			continue;
		}
		if (cn >= 'a' && cn <= 'z') cn = (char)(cn - 32);
		if (cp >= 'a' && cp <= 'z') cp = (char)(cp - 32);
		if (cp == '?' || cn == cp)
		{
			name++;
			pat++;
			continue;
		}
		if (star)
		{
			pat = star + 1;
			match++;
			name = match;
			continue;
		}
		return 0;
	}
	while (*pat == '*')
		pat++;
	return *pat == 0;
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
		if (pat && pat[0] && !fat_pat_match(inf.fname, pat))
			continue;
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

/* Structured listing (#621): f_readdir already reports the directory bit and
 * the file size, so the FILES size column costs no extra f_stat over USB. The
 * scan keeps the sorted-first `max` entries under mmb_dirent_cmp rather than
 * an arbitrary first-max cut, so the surviving set does not depend on FatFs
 * enumeration order (#676). */
int mmb_fat_list_entries(int letter, const char *dir, const char *pat,
			 mmb_dirent *out, int max, int *truncated)
{
	DIR dp;
	FILINFO inf;
	char full[160];
	int n = 0, total = 0;
	if (truncated)
		*truncated = 0;
	if (!out || max <= 0 || !mmb_fat_ready(letter))
		return -1;
	make_full(letter, dir, full, sizeof full);
	if (f_opendir(&dp, full) != FR_OK)
		return -1;
	for (;;)
	{
		mmb_dirent ent;

		if (f_readdir(&dp, &inf) != FR_OK || inf.fname[0] == 0)
			break;
		if (inf.fname[0] == '.')
			continue;
		if (pat && pat[0] && !fat_pat_match(inf.fname, pat))
			continue;
		memset(&ent, 0, sizeof(ent));
		strncpy(ent.name, inf.fname, sizeof(ent.name) - 1);
		ent.is_dir = (inf.fattrib & AM_DIR) ? 1 : 0;
		ent.size = ent.is_dir ? -1 : (int)inf.fsize;
		total++;
		mmb_dirent_offer(out, &n, max, &ent);
	}
	f_closedir(&dp);
	if (truncated)
		*truncated = total > max;
	return n;
}

}
