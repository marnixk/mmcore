/*
 * LN-01 link stubs: no physical volumes yet (the real POSIX backend lands in
 * LN-08). A: (ramdisk) lives entirely in mmbasic/src/vfs.c and still works;
 * C:-H: report "no media", so settings fall back to A:/.mmbasic.ini.
 */
#include "mmb_priv.h"

int mmb_fat_ready(int letter)
{
	(void)letter;
	return 0;
}

int mmb_fat_chdir(int letter, const char *path)
{
	(void)letter;
	(void)path;
	return -1;
}

int mmb_fat_mkdir(int letter, const char *path)
{
	(void)letter;
	(void)path;
	return -1;
}

int mmb_fat_rmdir(int letter, const char *path)
{
	(void)letter;
	(void)path;
	return -1;
}

int mmb_fat_unlink(int letter, const char *path)
{
	(void)letter;
	(void)path;
	return -1;
}

int mmb_fat_rename(int letter, const char *from, const char *to)
{
	(void)letter;
	(void)from;
	(void)to;
	return -1;
}

int mmb_fat_list(int letter, const char *dir, const char *pat, char *out, int outsz)
{
	(void)letter;
	(void)dir;
	(void)pat;
	if (out && outsz > 0)
		out[0] = 0;
	return -1;
}

int mmb_fat_write(int letter, const char *path, const void *data, unsigned n, int append)
{
	(void)letter;
	(void)path;
	(void)data;
	(void)n;
	(void)append;
	return -1;
}

void *mmb_fat_wopen(int letter, const char *path, int append)
{
	(void)letter;
	(void)path;
	(void)append;
	return 0;
}

int mmb_fat_wwrite(void *handle, const void *data, unsigned n)
{
	(void)handle;
	(void)data;
	(void)n;
	return -1;
}

int mmb_fat_wclose(void *handle)
{
	(void)handle;
	return -1;
}

int mmb_fat_read_at(int letter, const char *path, unsigned pos, void *data,
		    unsigned n, unsigned *got)
{
	(void)letter;
	(void)path;
	(void)pos;
	(void)data;
	(void)n;
	if (got)
		*got = 0;
	return -1;
}

int mmb_fat_size(int letter, const char *path)
{
	(void)letter;
	(void)path;
	return -1;
}

int mmb_fat_exists(int letter, const char *path)
{
	(void)letter;
	(void)path;
	return 0;
}

int mmb_fat_isdir(int letter, const char *path)
{
	(void)letter;
	(void)path;
	return 0;
}

const char *mmb_fat_cwd(int letter)
{
	(void)letter;
	return "/";
}

void mmb_fat_drive_line(int letter, char *out, int outsz)
{
	(void)letter;
	if (out && outsz > 0)
		out[0] = 0;
}

void mmb_storage_poll(void)
{
}

void mmb_storage_unmount(void)
{
}
