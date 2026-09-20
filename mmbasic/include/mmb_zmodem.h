#ifndef MMB_ZMODEM_H
#define MMB_ZMODEM_H

/*
 * ZMODEM receive-only engine for TERM downloads.
 *
 * The engine owns no transport: the caller feeds raw bytes and supplies file
 * and send callbacks through mmb_zm_ops.  It is self-contained (no mmb_priv.h
 * dependency) so the protocol can be exercised by a host unit test.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ZMODEM's maximum binary subpacket (ZMAXSPLEN) plus escape/CRC headroom. */
#define MMB_ZM_MAX_SUB  1200
#define MMB_ZM_MAX_NAME 128
#define MMB_ZM_STATUS   48

typedef struct {
	void *ctx;
	/* Prepare a destination for `name` (basename only, already sanitized).
	 * `size` is the advertised byte count, 0 when unknown.
	 * Return 0 on success, non-zero to skip the file. */
	int (*open)(void *ctx, const char *name, unsigned size);
	int (*write)(void *ctx, const unsigned char *data, unsigned n);
	/* ok != 0 keeps the file; ok == 0 removes the partial. */
	void (*close)(void *ctx, int ok);
	int (*send)(void *ctx, const unsigned char *data, unsigned n);
} mmb_zm_ops;

enum {
	MMB_ZM_IDLE = 0,
	MMB_ZM_ACTIVE,
	MMB_ZM_DONE,
	MMB_ZM_FAILED,
	MMB_ZM_CANCELLED
};

typedef struct mmb_zm_rx {
	const mmb_zm_ops *ops;
	int state;
	int ps;			/* parser state */
	int role;		/* what the pending subpacket belongs to */
	int hdr32;		/* last header used 32-bit FCS */
	int sub32;		/* pending subpacket uses 32-bit FCS */
	unsigned last_ms;
	unsigned bytes;		/* total bytes written this session */
	unsigned size;		/* current file advertised size */
	unsigned got;		/* current file bytes written */
	int files;		/* completed files */
	int file_open;
	int synced;		/* a valid header has been accepted */
	char name[MMB_ZM_MAX_NAME];
	char status[MMB_ZM_STATUS];
	int hdr_type;
	unsigned char hbody[16];
	int hex_n;
	int hex_byte;
	int bin_n;
	int bin_need;
	int bin_esc;
	int sub_n;
	int sub_end;		/* frame-end char of running subpacket */
	int sub_esc;
	unsigned char sub[MMB_ZM_MAX_SUB];
	int crc_n;
	int crc_need;
	unsigned char crcbuf[4];
	unsigned char rep[4];	/* last response header data bytes */
	int rep_type;		/* last response header type, -1 if none */
	int rep_count;
} mmb_zm_rx;

void mmb_zm_init(mmb_zm_rx *z, const mmb_zm_ops *ops);
void mmb_zm_begin(mmb_zm_rx *z);
void mmb_zm_feed(mmb_zm_rx *z, const unsigned char *data, unsigned n,
		 unsigned now_ms);
void mmb_zm_tick(mmb_zm_rx *z, unsigned now_ms);
void mmb_zm_cancel(mmb_zm_rx *z);
void mmb_zm_forget(mmb_zm_rx *z);
int mmb_zm_active(const mmb_zm_rx *z);
const char *mmb_zm_status(const mmb_zm_rx *z);
const char *mmb_zm_name(const mmb_zm_rx *z);
unsigned mmb_zm_progress(const mmb_zm_rx *z, unsigned *total);
int mmb_zm_files(const mmb_zm_rx *z);

#ifdef __cplusplus
}
#endif

#endif
