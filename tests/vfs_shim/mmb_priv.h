/* Minimal stand-in for mmb_priv.h so mmbasic/src/vfs.c can be compiled on the
 * host for the A: ramdisk streaming-write test. Only the symbols vfs.c needs
 * are declared here; the implementations live in tests/vfs_stream_host.c. */
#ifndef VFS_TEST_MMB_PRIV_H
#define VFS_TEST_MMB_PRIV_H

#include <stddef.h>
#include <string.h>

#include <stdint.h>

#define MMB_MAX_FILES     10
#define MMB_MAX_CONSOLES  4
#define MMB_MAX_DRIVES    8
#define MMB_ZIP_MAX_BYTES (16u * 1024u * 1024u)
#define T_STR             3

/* Structured directory entry (#621); mirrors mmbasic.h. */
#define MMB_DIRENT_NAME 256
typedef struct mmb_dirent {
	char name[MMB_DIRENT_NAME];
	int is_dir;
	int size;
} mmb_dirent;

/* Bounded sorted listing helpers (#676); mirrors mmbasic.h. */
int mmb_dirent_cmp(const mmb_dirent *a, const mmb_dirent *b);
void mmb_dirent_offer(mmb_dirent *heap, int *n, int max, const mmb_dirent *e);

typedef struct mmb_val {
	int type;
	double f;
	int64_t i;
	const char *s;
} mmb_val;

typedef struct mmb_file {
	int open;
	int mode;
	int pos;
	int kind;
	int ungot;
	char path[128];
} mmb_file;

typedef struct mmb_test_plat {
	void *(*alloc)(unsigned n);
	void (*free)(void *p);
} mmb_test_plat;

typedef struct mmb_test_globals {
	mmb_test_plat *plat;
	int drive;
	char cwd[128];
	int cwd_node[2];
	char cwd_path[MMB_MAX_DRIVES][128];
	mmb_file files[MMB_MAX_FILES + 1];
	const char *p;
	int running;
} mmb_test_globals;

extern mmb_test_globals G;
extern mmb_test_globals *g_mmb[MMB_MAX_CONSOLES];

int mmb_keyword_eq(const char *a, const char *b);

/* interpreter helpers used by the package/DRIVE path in vfs.c */
void mmb_error(const char *msg);
void mmb_skip_sp(void);
void mmb_syntax(void);
void mmb_out(const char *s);
void mmb_console_write(const char *s);
mmb_val mmb_expr(void);
int mmb_zip_foreach(const void *buf, unsigned n,
		    int (*cb)(const char *path, const void *data, unsigned len, void *ctx),
		    void *ctx);
int mmb_vfs_hidden_name(const char *name);
void mmb_vfs_drives(char *out, int outsz);
int mmb_vfs_chdir(const char *path);
int mmb_vfs_read(const char *path, void *data, unsigned maxn, unsigned *n);

/* Physical volumes (stubbed; the test only exercises A:). */
int mmb_fat_ready(int letter);
int mmb_fat_chdir(int letter, const char *path);
int mmb_fat_mkdir(int letter, const char *path);
int mmb_fat_rmdir(int letter, const char *path);
int mmb_fat_unlink(int letter, const char *path);
int mmb_fat_rename(int letter, const char *from, const char *to);
int mmb_fat_list(int letter, const char *dir, const char *pat, char *out, int outsz);
int mmb_fat_list_entries(int letter, const char *dir, const char *pat,
			 mmb_dirent *out, int max, int *truncated);
int mmb_fat_write(int letter, const char *path, const void *data, unsigned n, int append);
void *mmb_fat_wopen(int letter, const char *path, int append);
int mmb_fat_wwrite(void *handle, const void *data, unsigned n);
int mmb_fat_wclose(void *handle);
int mmb_fat_read_at(int letter, const char *path, unsigned pos, void *data, unsigned n, unsigned *got);
int mmb_fat_size(int letter, const char *path);
int mmb_fat_exists(int letter, const char *path);
int mmb_fat_isdir(int letter, const char *path);
const char *mmb_fat_cwd(int letter);
void mmb_fat_drive_line(int letter, char *out, int outsz);
int mmb_fat_eject(int letter);
int mmb_files_notice(const char *msg);

/* vfs.c public API under test. */
void mmb_vfs_init(void);
int mmb_vfs_write(const char *path, const void *data, unsigned n, int append);
int mmb_vfs_wopen(const char *path, int append);
int mmb_vfs_wwrite(int handle, const void *data, unsigned n);
int mmb_vfs_wclose(int handle);
int mmb_vfs_size(const char *path);
int mmb_vfs_read_at(const char *path, unsigned pos, void *data, unsigned n, unsigned *got);
int mmb_vfs_resolve(const char *path, char *out, int outsz);
int mmb_vfs_exists(const char *path);
const char *mmb_vfs_cwd(void);
int mmb_vfs_list_entries(const char *spec, mmb_dirent *out, int max,
			 int *truncated);

#endif
