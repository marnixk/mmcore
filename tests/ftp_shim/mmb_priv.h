/* Minimal stand-in for mmb_priv.h so tests/ftp_list_host.c can compile the
 * real mmbasic/src/cmd_ftp.c on the host. Only the symbols cmd_ftp.c uses are
 * declared here; the implementations live in tests/ftp_list_host.c. */
#ifndef FTP_TEST_MMB_PRIV_H
#define FTP_TEST_MMB_PRIV_H

#include <stddef.h>
#include <string.h>

typedef struct mmb_test_plat {
	void (*write_serial)(const char *s, unsigned n);
} mmb_test_plat;

typedef struct mmb_test_globals {
	mmb_test_plat *plat;
} mmb_test_globals;

extern mmb_test_globals G;

int mmb_vfs_resolve(const char *path, char *out, int outsz);
const char *mmb_vfs_cwd(void);
int mmb_vfs_isdir(const char *path);
int mmb_vfs_exists(const char *path);
int mmb_vfs_size(const char *path);
int mmb_vfs_read_at(const char *path, unsigned pos, void *data, unsigned n, unsigned *got);
int mmb_vfs_write(const char *path, const void *data, unsigned n, int append);
int mmb_vfs_wopen(const char *path, int append);
int mmb_vfs_wwrite(int handle, const void *data, unsigned n);
int mmb_vfs_wclose(int handle);
int mmb_vfs_kill(const char *path);
int mmb_vfs_mkdir(const char *path);
int mmb_vfs_rmdir(const char *path);
int mmb_vfs_rename(const char *src, const char *dst);
int mmb_vfs_list(const char *spec, char *out, int outsz);

int mmb_eth_start(void);
int mmb_net_available(void);
void mmb_net_yield(void);
unsigned mmb_now_ms(void);

int mmb_net_srv_listen(int port);
int mmb_net_srv_port(int lsn);
void mmb_net_srv_listen_close(int lsn);
int mmb_net_srv_accept(int lsn);
int mmb_net_srv_recv(int conn, void *data, unsigned maxn);
int mmb_net_srv_send(int conn, const void *data, unsigned n);
int mmb_net_srv_closed(int conn);
void mmb_net_srv_close(int conn);
int mmb_net_srv_ip(char *buf, int bufsize);

/* Public API implemented by mmbasic/src/cmd_ftp.c. */
int mmb_ftp_start(const char *root, int port);
void mmb_ftp_stop(void);
void mmb_ftp_poll(void);
int mmb_ftp_running(void);
int mmb_ftp_port(void);
const char *mmb_ftp_status(void);

#endif
