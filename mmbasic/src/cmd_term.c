#include "mmb_priv.h"
#include "mmb_zmodem.h"
#include "net_rxbuf.h"
#include "sixel.h"
#include "jxl_dec.h"
#include <string.h>

#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define GA    249
#define EL    248
#define EC    247
#define AYT   246
#define AO    245
#define IP    244
#define BRK   243
#define DM    242
#define NOP   241
#define SE    240
#define TELOPT_ECHO  1
#define TELOPT_SGA   3
#define TELOPT_TTYPE 24
#define TELOPT_NAWS  31
#define TTYPE_IS     0
#define TTYPE_SEND   1

#define TM_COLS_BOXED 80
#define TM_MAX_COLS   256
#define TM_CW         8
/* Default VGA glyph cell height. TERM lowers it to the EGA 8x14 face
 * (TM_CH_EGA) when a fullscreen 80x25 bookmark cannot fit 25 rows of 8x16
 * on the current display; see term_pick_cell_h(). */
#define TM_CH_VGA     16
#define TM_CH_EGA     14
#define TM_PAGE       2
#define TM_MAX_ROWS   80
#define TM_LINE     256
#define TM_ESC_BUF  16
#define TM_ANSI_ARGS 8

#define TM_DEMO_MIN_MS  120
#define TM_DEMO_MAX_MS  200
#define TM_ESC_IDLE_MS  60
#define TM_CONNECT_MS   25000
#define TM_RECV_MS      16
#define TM_RECV_LOOPS   256
#define TM_RECV_IDLE    8
#define TM_RECV_BUF     8192
#define TM_INTERP_YIELD 256
#define TM_DUMP_MS      250
#define TM_SB_MS        2000
#define TM_BM_MAX       32
#define TM_BM_NAME      40
#define TM_DLG_NONE     0
#define TM_DLG_LIST     1
#define TM_DLG_EDIT     2
#define TM_DLG_DEL      3
#define TM_DLG_DL       4
#define TM_DL_VIEW      11
#define TM_DL_USE       0
#define TM_DL_UP        1
#define TM_DL_CANCEL    2
#define TM_DL_LIST      3
#define TM_DL_DIRS      48
#define TM_MENU_ITEMS   6
#define TM_MENU_ROWS    8
#define TM_BTN_NEW      0
#define TM_BTN_EDIT     1
#define TM_BTN_DEL      2
#define TM_BTN_CONN     3
#define TM_LIST_VIEW    12
#define TM_ED_NAME      0
#define TM_ED_HOST      1
#define TM_ED_PORT      2
#define TM_ED_ECHO      3
#define TM_ED_BOX       4
#define TM_ED_80X25     5
#define TM_ED_CANCEL    6
#define TM_ED_SAVE      7
#define TM_MODE_80X25   2
#define TM_RS          0x1e
#define TM_BOX_H       0xC4u
#define TM_BOX_V       0xB3u
#define TM_BOX_TL      0xDAu
#define TM_BOX_TR      0xBFu
#define TM_BOX_BL      0xC0u
#define TM_BOX_BR      0xD9u
#define TM_ANSI_OSC_MS  2000
/* ansi_st values for DCS: parameters before the final byte, the body, and the
 * byte after an ESC that may be the string terminator. */
#define TM_ANSI_DCS_PARAM 8
#define TM_ANSI_DCS_BODY  9
#define TM_ANSI_DCS_ESC   10
#define TM_ANSI_APC_BODY  11
#define TM_ANSI_APC_ESC   12
#define TM_ANSI_STR_BODY  13
#define TM_ANSI_STR_ESC   14
/* Sixel/DCS framing. A full-screen image over a slow link can be silent for
 * longer than the OSC watchdog, so the DCS gets its own idle and body caps. */
#define TM_DCS_IDLE_MS  30000
#define TM_DCS_MAX      (4u * 1024u * 1024u)
#define TM_APC_IDLE_MS  30000
#define TM_APC_MAX      (8u * 1024u * 1024u)
#define TM_CACHE_ENTRIES 64
#define TM_CACHE_FILE_MAX (4u * 1024u * 1024u)
#define TM_CACHE_TOTAL_MAX (16u * 1024u * 1024u)
#define TM_CACHE_NAME   65
/* Decoded SyncTERM buffers live beside the cache: two paste buffers and one
 * mask buffer per console.  4 Mpx caps each (12 MB per paste buffer) so a
 * door cannot pin unbounded heap when it loads repeated frames. */
#define TM_PIXBUF_MAX_PIXELS (4u * 1024u * 1024u)
#define TM_MASK_MAX_PIXELS   (4u * 1024u * 1024u)
#define TM_PIXBUF_COUNT      2
#define TM_MASK_NONE   0
#define TM_MASK_INLINE 1
#define TM_MASK_FILE   2
#define TM_MASK_BUF    3
#define TM_REPLAY_HEX   512
#define TM_FILE_LINE    1280
#define TM_FILE_PEND    16
#define TM_LOG_BUF      32768
#define TM_LOG_FLUSH_MS 10000
#define TM_LOG_WATER    24576
#define TM_SB_MAX       256
#define TM_SB_COLS      TM_MAX_COLS
#define TM_SB_QUERY     64

typedef struct {
	char name[TM_CACHE_NAME];
	unsigned char *data;
	unsigned len;
	unsigned used;
	unsigned char md5[16];
	int live;
} term_cache_ent;

typedef struct {
	term_cache_ent e[TM_CACHE_ENTRIES];
	unsigned tick;
	unsigned total;
} term_cache;

typedef struct {
	int sx, sy, sw, sh;
	int dx, dy;
	int zx, zy;
	int fx, fy;
	int mx, my, mw, mh;
	int mask_kind;         /* TM_MASK_* */
	const char *mask_ptr;  /* inline MASK= base64, inside the APC body */
	unsigned mask_len;
	char mfile[TM_CACHE_NAME];
	int buf;               /* B= paste/load buffer selector */
} term_draw_opts;

typedef struct {
	int x, y, w, h;
	int buf;
} term_copy_opts;

typedef struct {
	int active;
	int demo;
	int demo_burst;
	int demo_iac;
	int demo_iac_step;
	int tcp;
	int connecting;
	unsigned connect_at;
	int net_fail;
	int net_lost;
	char net_msg[64];
	char host[80];
	int port;
	int saved_mode;
	int saved_bits;
	int pane_left;
	int pane_rows;
	int pane_cols;
	int letterbox;
	int mode80x25;
	int cell_h;            /* glyph cell height: 16 (VGA) or 14 (EGA) */
	int pane_top;          /* pane's vertical pixel offset (centring margin) */
	int vid_rows;
	int vid_cols;
	int cur_row;
	int cur_col;
	int cur_vis;
	int cur_shown;
	int cur_shown_row;
	char cell[TM_MAX_ROWS][TM_MAX_COLS];
	unsigned cell_fg[TM_MAX_ROWS][TM_MAX_COLS];
	unsigned cell_bg[TM_MAX_ROWS][TM_MAX_COLS];
	unsigned cur_fg;
	unsigned cur_bg;
	int ansi_bold;
	int ansi_inv;
	int ansi_ice;          /* SGR 5/6: high bit of a 40-47 background */
	int ansi_bg_base;      /* palette bg index 0-15, -1 for 256-colour RGB */
	int ansi_st;
	int ansi_narg;
	int ansi_arg[TM_ANSI_ARGS];
	int ansi_priv;
	unsigned dcs_at;       /* last DCS byte, for the idle abort */
	unsigned dcs_n;        /* DCS body bytes seen, for the cap */
	int dcs_sixel;         /* open DCS is a sixel command (q final) */
	unsigned apc_at;       /* last APC/PM/SOS byte, for the idle abort */
	unsigned apc_n;        /* APC/PM/SOS body bytes seen, for the cap */
	int apc_store;         /* 1 for APC (dispatch), 0 for PM/SOS */
	int six_on;            /* decoding a sixel image */
	int six_ok;            /* the plane is allocated and the decoder is live */
	int sixel_scroll;      /* DECSDM: default on, mode 80 set turns it off */
	int six_scrolled;      /* text rows scrolled while placing this image */
	int six_top;           /* visible top of the image, in plane pixels */
	int six_origin_row;
	sixel_image six;
	int img_dirty;         /* a sixel updated the plane since the last draw */
	int img_x, img_y, img_w, img_h;
	int img_lo, img_hi;    /* pane rows the image covers */
	int sav_row;
	int sav_col;
	int scroll_top;        /* DECSTBM scrolling region, 0-based inclusive */
	int scroll_bot;
	unsigned last_gch;     /* last graphic char printed, for REP (CSI b) */
	int char_mode;
	int no_echo;
	int echo_user;
	int iac;
	int iac_cmd;
	int sb;
	int sb_n;
	unsigned sb_at;
	int last_eol;
	int sb_opt;
	int sb_cmd;
	char line[TM_LINE];
	int linelen;
	int esc;
	int csi_n;
	char esc_buf[TM_ESC_BUF];
	int esc_len;
	unsigned esc_at;
	int alt;
	int alt_pend;
	int menu;
	int menu_sel;
	int replay;
	int replay_st;
	char replay_hex[TM_REPLAY_HEX + 4];
	int replay_hex_n;
	int file_replay;
	char file_path[88];
	int file_sz;
	unsigned file_pos;
	char file_line[TM_FILE_LINE];
	int file_line_n;
	int file_wait;
	int file_done;
	unsigned char file_pend[TM_FILE_PEND];
	int file_pend_n;
	int file_pct;          /* replay speed, percent (0 = instant) */
	int file_paused;
	int file_have_ms;
	unsigned file_last_ms;
	unsigned file_last_at;
	char file_rec[TM_FILE_LINE];
	int file_have_rec;
	int file_rec_timed;
	unsigned file_rec_ms;
	int sb_view;           /* lines scrolled back; 0 = live */
	int sb_search;         /* 1 while typing a search query */
	int sb_qn;
	char sb_query[TM_SB_QUERY];
	int sb_hl;             /* highlighted logical line, -1 none */
	unsigned dump_at;
	unsigned ansi_at;
	int mon_ansi;
	int mon_iac;
	int mon_sb;
	int dlg;
	int dlg_btn;
	int dlg_sel;
	int dlg_top;
	int dlg_focus;
	int dlg_edit_idx;
	char dlg_name[40];
	char dlg_host[80];
	int dlg_caret;
	int dlg_off;
	int dlg_port;
	int dlg_echo;
	int dlg_letterbox;
	int dlg_mode80x25;
	int demo_line;
	unsigned demo_next;
	int serial_gen;
	int need_draw;
	int dirty_full;
	int dirty_lo;
	int dirty_hi;
	int echo_typed;
	int live;
	unsigned rx_n;
	unsigned tx_n;
	unsigned parsed_n;
} tm_state;

static tm_state T_s[MMB_MAX_CONSOLES];
#define T (T_s[g_console])

/* A native-pixel plane beside the cell grid, one per console, allocated on
 * the first sixel.  Coverage records which cells the plane owns so
 * term_draw_row can composite them instead of glyphs.  The pixel buffer and
 * coverage bytes are one heap block; the struct lives beside it. */
typedef struct {
	uint16_t *pix;
	unsigned char *cov;
	int w, h, stride;
} term_plane;

static term_plane *P_s[MMB_MAX_CONSOLES];
#define P (*P_s[g_console])

/* SyncTERM C;Load* / P;Copy paste buffers (B=0/1) and the C;LoadPBM mask
 * buffer, one set per console.  Pixel buffers use the decoder's RGB888 layout
 * so a Load can move its result in without another copy. */
static mmb_img_rgb PB_s[MMB_MAX_CONSOLES][TM_PIXBUF_COUNT];
#define PB (PB_s[g_console])

typedef struct {
	unsigned char *bits;   /* tightly packed row-major, MSB-first, 1=draw */
	int w, h;
	int owned;             /* bits came from a temporary decode */
} term_mask;

static term_mask TMASK_s[MMB_MAX_CONSOLES];
#define TMASK (TMASK_s[g_console])

static void term_pixbuf_free_one(int idx, int b)
{
	if (PB_s[idx][b].rgb)
		free(PB_s[idx][b].rgb);
	PB_s[idx][b].rgb = NULL;
	PB_s[idx][b].w = 0;
	PB_s[idx][b].h = 0;
}

static void term_pixbuf_free_all(int idx)
{
	int b;

	for (b = 0; b < TM_PIXBUF_COUNT; b++)
		term_pixbuf_free_one(idx, b);
}

static void term_mask_free(int idx)
{
	if (TMASK_s[idx].bits)
		free(TMASK_s[idx].bits);
	TMASK_s[idx].bits = NULL;
	TMASK_s[idx].w = 0;
	TMASK_s[idx].h = 0;
}

static unsigned char *A_s[MMB_MAX_CONSOLES];
static unsigned A_cap_s[MMB_MAX_CONSOLES];
static term_cache *TC_s[MMB_MAX_CONSOLES];
#define A (A_s[g_console])
#define A_cap (A_cap_s[g_console])
#define TC (*TC_s[g_console])

/* Set while mmb_term_poll() runs from a background yield callback (#858): the
 * session is drained and interpreted, but nothing may be painted. */
static int s_term_bg;
static void term_bg_yield(int console, void *ctx);

/* Long TERM paths (paint, present, TCP ingest) must keep the mixer fed.
 * A full repaint can outlast the queued audio if the mixer waits for poll. */
static void term_yield(void)
{
	mmb_net_yield();
	mmb_play_mix();
}

static unsigned char term_rx_store[MMB_NET_RX_CAP];
static mmb_net_rxbuf term_rx;

typedef struct {
	unsigned in_n;
	unsigned out_n;
	unsigned rendered;
	unsigned flush_at;
	unsigned start_at;
	int buf_n;
	char buf[TM_LOG_BUF];
} term_log_state;

/* Scrollback: complete lines that have scrolled off the top of the pane.
 * A ring, so the newest TM_SB_MAX lines are always available.  Widths are
 * per line because Boxed/Full can change mid-session. */
typedef struct {
	int cap;          /* configured line count; 0 disables scrollback */
	int n;            /* stored lines (<= cap) */
	int head;         /* next write slot */
	unsigned short w[TM_SB_MAX];
	unsigned char ch[TM_SB_MAX][TM_SB_COLS];
	unsigned fg[TM_SB_MAX][TM_SB_COLS];
	unsigned bg[TM_SB_MAX][TM_SB_COLS];
} term_sb_state;

/* The log and scrollback rings are large (L ~32 KB, H ~576 KB each), so every
 * console gets its own heap copy on first TERM use instead of a 4x BSS array
 * that would not fit Circle's kernel window (#767). term_session_alloc()
 * allocates lazily; mmb_term_reset_all() frees every console's. */
static term_log_state *L_s[MMB_MAX_CONSOLES];
static term_sb_state *H_s[MMB_MAX_CONSOLES];
#define L (*L_s[g_console])
#define H (*H_s[g_console])

static int host_is_demo(void)
{
	return strcasecmp(T.host, "demo") == 0 ||
	       strcasecmp(T.host, "demoburst") == 0 ||
	       strcasecmp(T.host, "demoiac") == 0;
}

static int host_is_replay(void)
{
	return strcasecmp(T.host, "replay") == 0;
}

typedef struct {
	char name[TM_BM_NAME];
	char host[80];
	int port;
	int echo;
	int letterboxed;
	int mode80x25;
} term_bm;

static term_bm g_bm_s[MMB_MAX_CONSOLES][TM_BM_MAX];
static int g_bm_n_s[MMB_MAX_CONSOLES];
#define g_bm (g_bm_s[g_console])
#define g_bm_n (g_bm_n_s[g_console])
static int dlg_c0_s[MMB_MAX_CONSOLES], dlg_r0_s[MMB_MAX_CONSOLES];
static int dlg_cw_s[MMB_MAX_CONSOLES], dlg_ch_s[MMB_MAX_CONSOLES];
#define dlg_c0 (dlg_c0_s[g_console])
#define dlg_r0 (dlg_r0_s[g_console])
#define dlg_cw (dlg_cw_s[g_console])
#define dlg_ch (dlg_ch_s[g_console])
static unsigned char s_iac_out_s[MMB_MAX_CONSOLES][64];
static int s_iac_n_s[MMB_MAX_CONSOLES];
#define s_iac_out (s_iac_out_s[g_console])
#define s_iac_n (s_iac_n_s[g_console])

/* ZMODEM downloads. The download directory and the folder-browser cursor are
 * per-console session state (#670): switching consoles must not move the
 * folder another console is browsing. The persisted default is per-console
 * too (#679), so Use on one console no longer seeds every other console or
 * future boot. The transient browser list/selection state is per-console as
 * well (#680): opening the dialog on a second console must not rebuild the
 * arrays the first is drawing. The transfer engine, its scan window, the
 * in-flight destination and the last-download display are per-console too
 * (#767). The raw TCP socket stays a single machine resource shared by every
 * console (like JUKE/audio); it lives in console/net.cpp. */
static mmb_zm_rx ZM_s[MMB_MAX_CONSOLES];
static int zm_ready_s[MMB_MAX_CONSOLES];
static unsigned zm_shift_s[MMB_MAX_CONSOLES];
static char zm_shown_s[MMB_MAX_CONSOLES][80];
#define ZM (ZM_s[g_console])
#define zm_ready (zm_ready_s[g_console])
#define zm_shift (zm_shift_s[g_console])
#define zm_shown (zm_shown_s[g_console])
static char zm_cur_path_s[MMB_MAX_CONSOLES][128];
#define zm_cur_path (zm_cur_path_s[g_console])
static char g_dl_dir_s[MMB_MAX_CONSOLES][96];
static char g_dl_cur_s[MMB_MAX_CONSOLES][128];
#define g_dl_dir (g_dl_dir_s[g_console])
#define g_dl_cur (g_dl_cur_s[g_console])
static char g_dl_last_name_s[MMB_MAX_CONSOLES][MMB_ZM_MAX_NAME];
static int g_dl_last_files_s[MMB_MAX_CONSOLES];
#define g_dl_last_name (g_dl_last_name_s[g_console])
#define g_dl_last_files (g_dl_last_files_s[g_console])
static char g_dl_names_s[MMB_MAX_CONSOLES][TM_DL_DIRS][64];
static int g_dl_n_s[MMB_MAX_CONSOLES];
static int g_dl_sel_s[MMB_MAX_CONSOLES];
static int g_dl_top_s[MMB_MAX_CONSOLES];
static int g_dl_focus_s[MMB_MAX_CONSOLES];
static int g_dl_trunc_s[MMB_MAX_CONSOLES];
#define g_dl_names (g_dl_names_s[g_console])
#define g_dl_n (g_dl_n_s[g_console])
#define g_dl_sel (g_dl_sel_s[g_console])
#define g_dl_top (g_dl_top_s[g_console])
#define g_dl_focus (g_dl_focus_s[g_console])
#define g_dl_trunc (g_dl_trunc_s[g_console])

static int term_log_alloc(void)
{
	if (!L_s[g_console])
	{
		if (!G.plat || !G.plat->alloc)
			return -1;
		L_s[g_console] = (term_log_state *)G.plat->alloc(sizeof(term_log_state));
		if (!L_s[g_console])
			return -1;
		memset(L_s[g_console], 0, sizeof(term_log_state));
	}
	return 0;
}

static int term_session_alloc(void)
{
	if (term_log_alloc() != 0)
		return -1;
	if (!H_s[g_console])
	{
		if (!G.plat || !G.plat->alloc)
			return -1;
		H_s[g_console] = (term_sb_state *)G.plat->alloc(sizeof(term_sb_state));
		if (!H_s[g_console])
			return -1;
		memset(H_s[g_console], 0, sizeof(term_sb_state));
	}
	return 0;
}

static void term_sb_free(int idx)
{
	if (H_s[idx] && G.plat && G.plat->free)
		G.plat->free(H_s[idx]);
	H_s[idx] = 0;
}

static void term_cache_free(int idx);
static void term_apc_buf_free(int idx);
static void term_pixbuf_free_all(int idx);
static void term_mask_free(int idx);

static void term_session_free(int idx)
{
	if (L_s[idx] && G.plat && G.plat->free)
		G.plat->free(L_s[idx]);
	L_s[idx] = 0;
	term_sb_free(idx);
	term_cache_free(idx);
	term_apc_buf_free(idx);
	term_pixbuf_free_all(idx);
	term_mask_free(idx);
}

static void term_dl_open(void);
static void term_dl_draw(void);
static void term_dl_serial(void);
static int term_dl_key(char c);
static void term_dl_arrow(int c);
static void zmodem_draw_status(void);

static const mmb_ed_theme *term_th(void)
{
	return mmb_editor_theme();
}

static unsigned term_vga_rgb(int idx)
{
	static const unsigned pal[16] = {
		0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
		0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu,
		0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
		0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu
	};
	if (idx < 0)
		idx = 0;
	if (idx > 15)
		idx = 15;
	return pal[idx];
}

static unsigned term_rgb(unsigned char idx)
{
	const unsigned *pal = mmb_editor_palette();
	int i = (int)idx & 15;

	if (!pal)
		return term_vga_rgb(i);
	return pal[i] & 0xFFFFFFu;
}

#define TM_BG      0x000000u
#define TM_FG      0xAAAAAAu
#define TM_DIM     term_rgb(term_th()->cmt_fg)
#define TM_MENU_BG term_rgb(term_th()->menu_bg)
#define TM_MENU_FG term_rgb(term_th()->menu_fg)
#define TM_HOT     term_rgb(term_th()->hot)
#define TM_SEL_FG  term_rgb(term_th()->sel_fg)
#define TM_SEL_BG  term_rgb(term_th()->sel_bg)
#define TM_DLG_FG  term_rgb(term_th()->dlg_fg)
#define TM_DLG_BG  term_rgb(term_th()->dlg_bg)
#define TM_SH_FG   term_rgb(term_th()->sh_fg)
#define TM_SH_BG   term_rgb(term_th()->sh_bg)
#define TM_FIELD_FG term_rgb(term_th()->field_fg)
#define TM_FIELD_BG term_rgb(term_th()->field_bg)

static void term_draw(void);
static void term_draw_dlg(void);
static void term_serial_dump(void);
static void term_serial_dump_dlg(void);
static void mark_dirty_full(void);
static void term_layout(void);
static void term_fill_pages(void);
static int term_paint_cursor = 1;

/* Last glyph parameters painted into TM_PAGE for each pane cell (#331).
 * A dirty row only repaints cells whose packed ch/fg/bg key changed.  Keys
 * are never zero (ch is at least ' '), so a zeroed array forces a full row. */
static uint64_t term_cell_sh[TM_MAX_ROWS][TM_MAX_COLS];

static uint64_t term_shadow_key(unsigned ch, unsigned fg, unsigned bg)
{
	return (uint64_t)ch | ((uint64_t)(fg & 0xFFFFFFu) << 8) |
	       ((uint64_t)(bg & 0xFFFFFFu) << 32);
}

static void pane_clear_row(int row);
static void term_plane_uncover_cell(int r, int c);
static void term_plane_uncover_range(int r, int c0, int c1);
static void pane_puts(const char *s);
static void pane_newline(void);
static void tcp_close_quiet(void);
static void tcp_lost(const char *why);
static void replay_closed(const char *why);
static void telnet_announce(void);
static void term_reset_pen(void);
static void demo_emit_line(void);
static void esc_reset(void);
static int term_width(void);
static int term_tcp_drain(int idle_max);
static void incoming_feed(const unsigned char *src, int n);
static int term_rx_interpret(void);
static void file_replay_poll(void);
static void term_open_menu(void);
static void term_close_menu(void);
static void term_dlg_refresh(void);
static void term_overlay_chrome(void);
static void term_log_mark(void);
static void term_log_rx(const unsigned char *p, int n);
static void term_log_tx(const unsigned char *p, int n);
static void term_log_event(const char *s);
static void term_log_flush(void);
static void term_log_poll(void);
static void term_log_path(char *dst, unsigned n);
static void term_mark_live(void);
static void term_net_stats(const char *ev);

static int term_want_echo(void);
static void pane_rubout(void);
static void term_echo_byte(unsigned char b);
static void send_naws(void);
static int term_status_y(void);
static void term_cell(int x, int y, unsigned ch, unsigned fg, unsigned bg);
static void term_put_str_bg(int x, int y, const char *s, unsigned fg, unsigned bg);

/* ---- scrollback ------------------------------------------------------ */

static int sb_total(void)
{
	return H.n + T.cur_row + 1;
}

static int sb_max_view(void)
{
	int m = sb_total() - T.pane_rows;

	return m > 0 ? m : 0;
}

static void sb_slot_line(int line, int c, unsigned *ch, unsigned *fg, unsigned *bg)
{
	int slot;

	if (line < 0 || line >= H.n)
	{
		*ch = ' ';
		*fg = TM_FG;
		*bg = TM_BG;
		return;
	}
	slot = (H.head - H.n + line) % H.cap;
	if (slot < 0)
		slot += H.cap;
	if (c < 0 || c >= (int)H.w[slot])
	{
		*ch = ' ';
		*fg = TM_FG;
		*bg = TM_BG;
		return;
	}
	*ch = H.ch[slot][c];
	*fg = H.fg[slot][c];
	*bg = H.bg[slot][c];
}

static void term_view_cell(int r, int c, unsigned *ch, unsigned *fg, unsigned *bg)
{
	int line, top;

	if (T.sb_view <= 0)
	{
		*ch = (unsigned char)T.cell[r][c];
		*fg = T.cell_fg[r][c];
		*bg = T.cell_bg[r][c];
		return;
	}
	top = sb_total() - T.pane_rows - T.sb_view;
	line = top + r;
	if (line < 0)
	{
		*ch = ' ';
		*fg = TM_FG;
		*bg = TM_BG;
		return;
	}
	if (line < H.n)
		sb_slot_line(line, c, ch, fg, bg);
	else
	{
		int lr = line - H.n;

		if (lr >= 0 && lr < T.pane_rows)
		{
			*ch = (unsigned char)T.cell[lr][c];
			*fg = T.cell_fg[lr][c];
			*bg = T.cell_bg[lr][c];
		}
		else
		{
			*ch = ' ';
			*fg = TM_FG;
			*bg = TM_BG;
		}
	}
	if (line == T.sb_hl)
	{
		unsigned t = *fg;

		*fg = *bg;
		*bg = t;
	}
}

static void sb_push(const unsigned char *ch, const unsigned *fg, const unsigned *bg,
		    int w)
{
	int slot, c, grew;

	if (H.cap <= 0)
		return;
	if (w < 0)
		w = 0;
	if (w > TM_SB_COLS)
		w = TM_SB_COLS;
	slot = H.head;
	for (c = 0; c < w; c++)
	{
		H.ch[slot][c] = ch[c];
		H.fg[slot][c] = fg[c];
		H.bg[slot][c] = bg[c];
	}
	H.w[slot] = (unsigned short)w;
	H.head = (H.head + 1) % H.cap;
	grew = H.n < H.cap;
	if (grew)
		H.n++;
	/* Pin the viewed text while new output arrives. */
	if (T.sb_view > 0 && grew && T.sb_view < sb_max_view())
		T.sb_view++;
}

static int sb_line_text(int line, char *out, int maxn)
{
	int c, w;

	if (line < 0)
		return 0;
	if (line < H.n)
	{
		int slot = (H.head - H.n + line) % H.cap;

		if (slot < 0)
			slot += H.cap;
		w = (int)H.w[slot];
		if (w > maxn - 1)
			w = maxn - 1;
		for (c = 0; c < w; c++)
			out[c] = (char)H.ch[slot][c];
	}
	else
	{
		int lr = line - H.n;

		if (lr < 0 || lr >= T.pane_rows)
			return 0;
		w = term_width();
		if (w > maxn - 1)
			w = maxn - 1;
		for (c = 0; c < w; c++)
			out[c] = T.cell[lr][c] ? T.cell[lr][c] : ' ';
	}
	out[w] = 0;
	while (w > 0 && out[w - 1] == ' ')
		out[--w] = 0;
	return 1;
}

static int sb_ci_contains(const char *h, const char *n)
{
	int i, j;

	if (!n[0])
		return 0;
	for (i = 0; h[i]; i++)
	{
		for (j = 0; n[j]; j++)
		{
			char a = h[i + j], b = n[j];

			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (!n[j])
			return 1;
	}
	return 0;
}

static int sb_find(int from, int dir)
{
	int line, total = sb_total();
	char txt[TM_SB_COLS + 1];

	for (line = from; line >= 0 && line < total; line += dir)
		if (sb_line_text(line, txt, (int)sizeof(txt)) &&
		    sb_ci_contains(txt, T.sb_query))
			return line;
	return -1;
}

static void sb_focus_line(int line)
{
	int total = sb_total();
	int view = total - 1 - (line + T.pane_rows - 1);

	if (view < 0)
		view = 0;
	if (view > sb_max_view())
		view = sb_max_view();
	T.sb_view = view;
	T.sb_hl = line;
}

static void sb_refresh(void)
{
	mark_dirty_full();
	if (T.need_draw)
		term_draw();
	term_serial_dump();
}

static void sb_scroll_by(int delta)
{
	int max = sb_max_view();

	T.sb_view += delta;
	if (T.sb_view < 0)
		T.sb_view = 0;
	if (T.sb_view > max)
		T.sb_view = max;
	T.sb_hl = -1;
	sb_refresh();
}

static void sb_to_live(void)
{
	T.sb_view = 0;
	T.sb_hl = -1;
	T.sb_search = 0;
	sb_refresh();
}

static void sb_search_begin(void)
{
	T.sb_search = 1;
	T.sb_qn = 0;
	T.sb_query[0] = 0;
	sb_refresh();
}

static void sb_search_run(void)
{
	int line, bottom = sb_total() - 1 - (T.sb_view > 0 ? T.sb_view : 0);

	T.sb_search = 0;
	if (!T.sb_query[0])
	{
		sb_refresh();
		return;
	}
	line = sb_find(bottom, -1);
	if (line < 0)
		line = sb_find(sb_total() - 1, -1);
	if (line >= 0)
		sb_focus_line(line);
	sb_refresh();
}

static void sb_search_step(int dir)
{
	int line;

	if (!T.sb_query[0])
		return;
	line = (T.sb_hl >= 0) ? sb_find(T.sb_hl + dir, dir)
			      : sb_find(sb_total() - 1, -1);
	if (line >= 0)
		sb_focus_line(line);
	sb_refresh();
}

static void sb_status_number(char *dst, unsigned n)
{
	char tmp[12];
	int i = 0;

	if (n == 0)
		tmp[i++] = '0';
	while (n && i < 11)
	{
		tmp[i++] = (char)('0' + (n % 10));
		n /= 10;
	}
	while (i > 0)
	{
		int len = (int)strlen(dst);

		if (len + 2 >= 48)
			break;
		dst[len] = tmp[--i];
		dst[len + 1] = 0;
	}
}

static void term_draw_sb_status(void)
{
	char text[96];
	unsigned bg = TM_MENU_BG, fg = TM_MENU_FG;
	int y = term_status_y();

	mmb_gfx_fill_rect_topdown(0, y, T.vid_cols * TM_CW, T.cell_h, bg);
	if (T.sb_search)
	{
		strcpy(text, "Search: ");
		strncat(text, T.sb_query, 40);
		term_put_str_bg(0, y, text, fg, bg);
		term_cell((int)strlen(text) * TM_CW, y, '_', TM_HOT, bg);
		return;
	}
	text[0] = 0;
	if (T.sb_view > 0)
	{
		strcpy(text, "SCROLL -");
		sb_status_number(text, (unsigned)T.sb_view);
		strncat(text, " ", sizeof(text) - 1 - strlen(text));
	}
	if (T.sb_query[0] && T.sb_hl >= 0)
		strncat(text, "n older  N newer  / search  q live",
			sizeof(text) - 1 - strlen(text));
	else
		strncat(text, "PgUp/PgDn  / search  q live",
			sizeof(text) - 1 - strlen(text));
	term_put_str_bg(0, y, text, fg, bg);
}

void mmb_term_scrollback_set(int lines)
{
	if (!H_s[g_console])
		return;
	if (lines < 0)
		lines = 0;
	if (lines > TM_SB_MAX)
		lines = TM_SB_MAX;
	H.cap = lines;
	if (H.n > H.cap)
	{
		H.n = 0;
		H.head = 0;
	}
	if (T.sb_view > sb_max_view())
		T.sb_view = 0;
}

static unsigned ansi_pal(int n)
{
	static const unsigned pal[16] = {
		0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
		0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu,
		0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
		0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu
	};
	if (n < 0)
		n = 0;
	if (n > 15)
		n = 15;
	return pal[n];
}

static unsigned ansi_256(int n)
{
	int r, g, b, v;
	if (n < 16)
		return ansi_pal(n);
	if (n < 232)
	{
		n -= 16;
		b = n % 6;
		g = (n / 6) % 6;
		r = n / 36;
		r = r ? r * 40 + 55 : 0;
		g = g ? g * 40 + 55 : 0;
		b = b ? b * 40 + 55 : 0;
		return mmb_rgb_pack(r, g, b);
	}
	v = 8 + (n - 232) * 10;
	if (v > 255)
		v = 255;
	return mmb_rgb_pack(v, v, v);
}

static unsigned term_pen(void)
{
	unsigned fg = T.cur_fg;
	if (T.ansi_bold)
	{
		int r = (int)((fg >> 16) & 255);
		int g = (int)((fg >> 8) & 255);
		int b = (int)(fg & 255);
		r += (255 - r) / 3;
		g += (255 - g) / 3;
		b += (255 - b) / 3;
		fg = mmb_rgb_pack(r, g, b);
	}
	return T.ansi_inv ? T.cur_bg : fg;
}

static unsigned term_paper(void)
{
	return T.ansi_inv ? T.cur_fg : T.cur_bg;
}

/* Apply the iCE high bit to the selected palette background. SGR 5 is the
 * blink attribute on a real VGA, but every BBS/ANSI file here wants the 16
 * background colours it unlocks instead of blinking text. */
static void ansi_update_bg(void)
{
	int idx;
	if (T.ansi_bg_base < 0)
		return;
	idx = T.ansi_bg_base;
	if (T.ansi_ice && idx < 8)
		idx += 8;
	T.cur_bg = ansi_pal(idx);
}

static void term_reset_pen(void)
{
	T.cur_fg = TM_FG;
	T.cur_bg = TM_BG;
	T.ansi_bold = 0;
	T.ansi_inv = 0;
	T.ansi_ice = 0;
	T.ansi_bg_base = 0;
}

/* The USB keyboard delivers Alt+key as Ctrl+A then the key, so a byte 1 on
 * its own is ambiguous. While a real Alt is held it is an Alt chord (local
 * menu/exit); otherwise it is a genuine Ctrl+A and belongs to the remote
 * (#1218). */
static int term_alt_held(void)
{
	return (G.plat && G.plat->alt_held && G.plat->alt_held()) ? 1 : 0;
}

static void ser(const char *s)
{
	unsigned n;
	if (!s || !G.plat || !G.plat->write_serial)
		return;
	n = (unsigned)strlen(s);
	if (n)
		G.plat->write_serial(s, n);
}

static void fmt_hostport(char *dst, unsigned maxn)
{
	unsigned n = 0;
	const char *h = T.host;
	char pbuf[8];
	int pi = 0, pv = T.port;

	while (h[n] && n + 1 < maxn)
	{
		dst[n] = h[n];
		n++;
	}
	if (n + 1 < maxn)
		dst[n++] = ':';
	if (pv == 0)
		pbuf[pi++] = '0';
	else
	{
		while (pv && pi < 7)
		{
			pbuf[pi++] = (char)('0' + (pv % 10));
			pv /= 10;
		}
	}
	while (pi > 0 && n + 1 < maxn)
		dst[n++] = pbuf[--pi];
	dst[n] = 0;
}

static void fmt_line_num(char *dst, int n)
{
	dst[0] = 'l';
	dst[1] = 'i';
	dst[2] = 'n';
	dst[3] = 'e';
	dst[4] = ' ';
	dst[5] = (char)('0' + (n / 10) % 10);
	dst[6] = (char)('0' + n % 10);
	dst[7] = 0;
}

static int term_width(void)
{
	return T.pane_cols > 0 ? T.pane_cols : TM_COLS_BOXED;
}

static const char *term_width_label(void)
{
	if (T.mode80x25)
		return "View: 80x25";
	return T.letterbox ? "View: Boxed" : "View: Full";
}

static void term_mark_live(void)
{
	T.live = 1;
	T.rx_n = 0;
	T.tx_n = 0;
	T.parsed_n = 0;
}

static void ser_u(unsigned v)
{
	char tmp[12];
	int i;

	if (v == 0)
	{
		ser("0");
		return;
	}
	i = (int)sizeof(tmp) - 1;
	tmp[i] = 0;
	while (v && i > 0)
	{
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	}
	ser(tmp + i);
}

static void term_net_stats(const char *ev)
{
	if (!mmb_opt_console_serial())
		return;
	ser("!NET ");
	if (ev && ev[0])
	{
		ser(ev);
		ser(" ");
	}
	ser("rx=");
	ser_u(T.rx_n);
	ser(" tx=");
	ser_u(T.tx_n);
	ser(" parsed=");
	ser_u(T.parsed_n);
	ser(" ring=");
	ser_u(mmb_net_rxbuf_used(&term_rx));
	ser(" a=");
	ser_u((unsigned)T.ansi_st);
	ser(" i=");
	ser_u((unsigned)T.iac);
	ser(" s=");
	ser_u(T.sb ? 1u : 0u);
	ser(" e=");
	ser_u(term_want_echo() ? 1u : 0u);
	ser(" c=");
	ser_u(T.char_mode ? 1u : 0u);
	ser("\r\n");
}

static void term_serial_dump(void)
{
	int r, c, cols;
	char line[TM_MAX_COLS + 1];

	if (!T.active)
		return;
	if (T.file_replay || T.live)
		return;
	if (!mmb_opt_console_serial())
		return;
	cols = term_width();
	for (r = 0; r < T.pane_rows; r++)
	{
		for (c = 0; c < cols; c++)
		{
			unsigned ch, fg, bg;

			term_view_cell(r, c, &ch, &fg, &bg);
			line[c] = ch ? (char)ch : ' ';
		}
		line[cols] = 0;
		ser(line);
		ser("\r\n");
		mmb_play_mix();
	}
	if (T.sb_search)
	{
		ser("Search: ");
		ser(T.sb_query);
		ser("\r\n");
	}
	else if (T.sb_view > 0)
	{
		char sbuf[32];

		strcpy(sbuf, "SCROLL -");
		sb_status_number(sbuf, (unsigned)T.sb_view);
		ser(sbuf);
		ser("\r\n");
	}
	if (T.sb_hl >= 0 && T.sb_query[0])
	{
		ser("MATCH ");
		ser(T.sb_query);
		ser("\r\n");
	}
	if (T.menu || T.alt_pend)
	{
		ser("Terminal\r\n");
		ser("Bookmarks\r\n");
		ser("Downloads Folder\r\n");
		ser("----------------\r\n");
		ser(term_width_label());
		ser("\r\n");
		ser(term_want_echo() ? "Echo [x]\r\n" : "Echo [ ]\r\n");
		ser(G.opt.term_scroll_buffer ? "Scrollback [x]\r\n"
					     : "Scrollback [ ]\r\n");
		ser("----------------\r\n");
		ser("Exit\r\n");
	}
	term_serial_dump_dlg();
	T.serial_gen++;
}

/* Pick the glyph face for this session. An 80x25 bookmark wants 25 rows of
 * 8x16, which needs MM.VRES >= 400. Judge that from the display's own mode
 * (the console mode TERM started from, T.saved_mode) rather than TERM's
 * current session mode, so TERM's own Boxed/Full switch cannot mask a short
 * panel. A 1366x768 Chromebook whose 2x logical mode is MODE 20 (683x384)
 * gives only 24 rows of 8x16, so fall back to the EGA 8x14 face, which needs
 * just 350, and keep the native mode. Every other session, display and app
 * stays on 8x16. */
static void term_pick_cell_h(void)
{
	int h = mmb_gfx_mode_h(T.saved_mode);

	if (h <= 0)
		h = G.gfx.h;
	T.cell_h = TM_CH_VGA;
	if (T.mode80x25 && h > 0 && h / 16 < 25)
		T.cell_h = TM_CH_EGA;
}

static void term_layout(void)
{
	int fb_h = G.gfx.h > 0 ? G.gfx.h : 400;

	T.vid_cols = G.plat && G.plat->video_cols ? G.plat->video_cols() : 80;
	if (T.cell_h == TM_CH_EGA)
		T.vid_rows = fb_h / T.cell_h;
	else
		T.vid_rows = G.plat && G.plat->video_rows ? G.plat->video_rows() : 33;
	T.pane_top = 0;
	if (T.mode80x25)
	{
		T.pane_cols = TM_COLS_BOXED;
		if (T.pane_cols > T.vid_cols)
			T.pane_cols = T.vid_cols;
		if (T.cell_h == TM_CH_EGA)
		{
			/* Centre an exact 80x25 grid; the leftover rows and
			 * columns become margin (#1155). */
			T.pane_rows = T.vid_rows < 25 ? T.vid_rows : 25;
			if (T.pane_cols != TM_COLS_BOXED)
				T.pane_rows = T.vid_rows;
			T.pane_left = (T.vid_cols - T.pane_cols) / 2;
			if (T.pane_left < 0)
				T.pane_left = 0;
			T.pane_top = (fb_h - T.pane_rows * T.cell_h) / 2;
			if (T.pane_top < 0)
				T.pane_top = 0;
		}
		else
		{
			T.pane_left = 0;
			T.pane_rows = T.vid_rows;
		}
	}
	else
	{
		if (T.letterbox)
			T.pane_cols = TM_COLS_BOXED;
		else
			T.pane_cols = T.vid_cols;
		if (T.pane_cols > TM_MAX_COLS)
			T.pane_cols = TM_MAX_COLS;
		if (T.pane_cols < 1)
			T.pane_cols = 1;
		T.pane_left = (T.vid_cols - T.pane_cols) / 2;
		if (T.pane_left < 0)
			T.pane_left = 0;
		T.pane_rows = T.vid_rows - 1;
	}
	if (T.pane_cols > TM_MAX_COLS)
		T.pane_cols = TM_MAX_COLS;
	if (T.pane_cols < 1)
		T.pane_cols = 1;
	if (T.pane_rows < 1)
		T.pane_rows = 1;
	if (T.pane_rows > TM_MAX_ROWS)
		T.pane_rows = TM_MAX_ROWS;
}

static int term_fb_h(void)
{
	int h = G.gfx.h;
	int hh;

	if (h < 1)
		h = T.vid_rows * T.cell_h;
	if (G.plat && G.plat->hdmi_height)
	{
		hh = G.plat->hdmi_height();
		if (hh > 0 && hh < h)
			h = hh;
	}
	if (h < T.cell_h)
		h = T.cell_h;
	return h;
}

static int term_status_y(void)
{
	return term_fb_h() - T.cell_h;
}

static void pane_clear_row(int row)
{
	int c;
	if (row < 0 || row >= T.pane_rows)
		return;
	for (c = 0; c < term_width(); c++)
	{
		T.cell[row][c] = ' ';
		T.cell_fg[row][c] = TM_FG;
		T.cell_bg[row][c] = TM_BG;
	}
	term_plane_uncover_range(row, 0, term_width());
}

/* Erase a row to the current SGR pen, so erased cells keep the active
 * background (Background Color Erase). Used by ED and by the blank line a
 * scroll lifts in; EL and ECH already do this per cell. (#1219) */
static void pane_erase_row_pen(int row)
{
	int c;
	if (row < 0 || row >= T.pane_rows)
		return;
	for (c = 0; c < term_width(); c++)
	{
		T.cell[row][c] = ' ';
		T.cell_fg[row][c] = term_pen();
		T.cell_bg[row][c] = term_paper();
	}
	term_plane_uncover_range(row, 0, term_width());
}

static void mark_dirty_row(int row)
{
	if (row < 0 || row >= T.pane_rows)
		return;
	T.need_draw = 1;
	if (T.dirty_full)
		return;
	if (T.dirty_lo < 0)
	{
		T.dirty_lo = row;
		T.dirty_hi = row;
		return;
	}
	if (row < T.dirty_lo)
		T.dirty_lo = row;
	if (row > T.dirty_hi)
		T.dirty_hi = row;
}

static void mark_dirty_full(void)
{
	T.dirty_full = 1;
	T.need_draw = 1;
}

/* ---- sixel plane ----------------------------------------------------- */

static void term_plane_free(int idx)
{
	term_plane *p = P_s[idx];

	if (!p)
		return;
	if (G.plat && G.plat->free)
	{
		G.plat->free(p->pix);
		G.plat->free(p);
	}
	P_s[idx] = 0;
}

static int term_plane_alloc(void)
{
	term_plane *p;
	void *block;
	unsigned pixbytes, covbytes;
	int w, h;

	if (P_s[g_console])
		return 0;
	if (!G.plat || !G.plat->alloc)
		return -1;
	w = term_width() * TM_CW;
	h = T.pane_rows * T.cell_h;
	if (w < 1 || h < 1)
		return -1;
	pixbytes = (unsigned)w * (unsigned)h * sizeof(uint16_t);
	covbytes = TM_MAX_ROWS * TM_MAX_COLS;
	p = (term_plane *)G.plat->alloc((unsigned)sizeof(term_plane));
	block = G.plat->alloc(pixbytes + covbytes);
	if (!p || !block)
	{
		if (p && G.plat->free)
			G.plat->free(p);
		if (block && G.plat->free)
			G.plat->free(block);
		return -1;
	}
	memset(block, 0, pixbytes + covbytes);
	p->pix = (uint16_t *)block;
	p->cov = (unsigned char *)block + pixbytes;
	p->w = w;
	p->h = h;
	p->stride = w;
	P_s[g_console] = p;
	return 0;
}

static void term_plane_clear(void)
{
	if (!P_s[g_console])
		return;
	memset(P.pix, 0, (size_t)P.w * (size_t)P.h * sizeof(uint16_t));
	memset(P.cov, 0, TM_MAX_ROWS * TM_MAX_COLS);
}

/* Scroll pixels and coverage inside [top,bot], matching the cell scroll.
 * Rows that leave the region are dropped. */
static void term_plane_scroll_rows(int top, int bot, int n, int up)
{
	term_plane *p = P_s[g_console];
	int rows, r;

	if (!p)
		return;
	if (top < 0)
		top = 0;
	if (bot >= T.pane_rows)
		bot = T.pane_rows - 1;
	rows = bot - top + 1;
	if (rows < 1)
		return;
	if (n < 1)
		n = 1;
	if (n > rows)
		n = rows;
	if (up)
	{
		if (n < rows)
			memmove(p->pix + top * T.cell_h * p->stride,
				p->pix + (top + n) * T.cell_h * p->stride,
				(size_t)(rows - n) * T.cell_h * p->stride *
					sizeof(uint16_t));
		memset(p->pix + (bot - n + 1) * T.cell_h * p->stride, 0,
		       (size_t)n * T.cell_h * p->stride * sizeof(uint16_t));
		for (r = top; r <= bot - n; r++)
			memmove(p->cov + r * TM_MAX_COLS,
				p->cov + (r + n) * TM_MAX_COLS, TM_MAX_COLS);
		for (r = bot - n + 1; r <= bot; r++)
			memset(p->cov + r * TM_MAX_COLS, 0, TM_MAX_COLS);
	}
	else
	{
		if (n < rows)
			memmove(p->pix + (top + n) * T.cell_h * p->stride,
				p->pix + top * T.cell_h * p->stride,
				(size_t)(rows - n) * T.cell_h * p->stride *
					sizeof(uint16_t));
		memset(p->pix + top * T.cell_h * p->stride, 0,
		       (size_t)n * T.cell_h * p->stride * sizeof(uint16_t));
		for (r = bot; r >= top + n; r--)
			memmove(p->cov + r * TM_MAX_COLS,
				p->cov + (r - n) * TM_MAX_COLS, TM_MAX_COLS);
		for (r = top; r < top + n; r++)
			memset(p->cov + r * TM_MAX_COLS, 0, TM_MAX_COLS);
	}
}

static void term_plane_scroll_up(int n)
{
	term_plane_scroll_rows(T.scroll_top, T.scroll_bot, n, 1);
}

/* Clear one cell's coverage so the glyph path owns it again.  The shadow key
 * is zeroed so the next glyph paints even if the character did not change. */
static void term_plane_uncover_cell(int r, int c)
{
	if (!P_s[g_console] || r < 0 || r >= T.pane_rows || c < 0 ||
	    c >= term_width())
		return;
	P.cov[r * TM_MAX_COLS + c] = 0;
	term_cell_sh[r][c] = 0;
}

static void term_plane_uncover_range(int r, int c0, int c1)
{
	int c;

	for (c = c0; c < c1; c++)
		term_plane_uncover_cell(r, c);
}

/* ICH/DCH: shift one row's pixels and coverage by n cells. */
static void term_plane_shift_cols(int r, int col, int n, int right)
{
	term_plane *p = P_s[g_console];
	int w, dy, i;
	unsigned char *cov;

	if (!p || r < 0 || r >= T.pane_rows)
		return;
	w = term_width();
	if (col < 0)
		col = 0;
	if (col >= w)
		return;
	if (n < 1)
		n = 1;
	if (n > w - col)
		n = w - col;
	if (n <= 0)
		return;
	cov = p->cov + r * TM_MAX_COLS;
	for (dy = 0; dy < T.cell_h; dy++)
	{
		uint16_t *line = p->pix + (r * T.cell_h + dy) * p->stride;

		if (right)
		{
			if (w - col - n > 0)
				memmove(line + (col + n) * TM_CW, line + col * TM_CW,
					(size_t)(w - col - n) * TM_CW *
						sizeof(uint16_t));
		}
		else
		{
			if (w - col - n > 0)
				memmove(line + col * TM_CW, line + (col + n) * TM_CW,
					(size_t)(w - col - n) * TM_CW *
						sizeof(uint16_t));
		}
	}
	if (right)
	{
		if (w - col - n > 0)
			memmove(cov + col + n, cov + col, (size_t)(w - col - n));
		memset(cov + col, 0, (size_t)n);
	}
	else
	{
		if (w - col - n > 0)
			memmove(cov + col, cov + col + n, (size_t)(w - col - n));
		memset(cov + (w - n), 0, (size_t)n);
	}
	for (i = 0; i < n; i++)
	{
		int c = right ? col + i : w - n + i;

		if (c >= 0 && c < w)
			term_cell_sh[r][c] = 0;
	}
}

/* Composite one run of covered cells from the plane into page 2. */
static void term_plane_blit(int r, int c0, int c1)
{
	uint16_t *page;
	int bw, bh, dy;
	int x, y;
	int n = c1 - c0;

	if (!P_s[g_console])
		return;
	page = mmb_gfx_buf_for(TM_PAGE, &bw, &bh);
	if (!page)
		return;
	x = (T.pane_left + c0) * TM_CW;
	y = T.pane_top + r * T.cell_h;
	for (dy = 0; dy < T.cell_h; dy++)
	{
		if (y + dy >= bh)
			break;
		memcpy(page + (y + dy) * bw + x,
		       P.pix + (r * T.cell_h + dy) * P.stride + c0 * TM_CW,
		       (size_t)n * TM_CW * sizeof(uint16_t));
	}
}

static void term_draw(void);
static void term_draw_row(int r);

static void term_set_cursor_vis(int vis)
{
	vis = vis ? 1 : 0;
	if (T.cur_vis == vis)
		return;
	T.cur_vis = vis;
	mark_dirty_row(T.cur_row);
	if (T.cur_shown)
		mark_dirty_row(T.cur_shown_row);
}

/* #1225: DECSTBM scrolling region.  scroll_top/scroll_bot are 0-based and
 * inclusive; the default covers the whole pane.  It is (re)set explicitly
 * whenever the pane is (re)laid out, since a zeroed struct would otherwise
 * look like a one-row region at row 0. */
static void term_reset_scroll_region(void)
{
	T.scroll_top = 0;
	T.scroll_bot = T.pane_rows > 0 ? T.pane_rows - 1 : 0;
}

static void pane_scroll_up(void)
{
	int r, c, top = T.scroll_top, bot = T.scroll_bot;

	if (bot <= top)
		return;
	term_plane_scroll_up(1);
	/* Only a region anchored at the pane top spills into scrollback; a
	 * sub-region scroll discards the line at its top margin. (#1225) */
	if (top == 0)
		sb_push((const unsigned char *)T.cell[0], T.cell_fg[0],
			T.cell_bg[0], term_width());
	for (r = top; r < bot; r++)
	{
		for (c = 0; c < term_width(); c++)
		{
			T.cell[r][c] = T.cell[r + 1][c];
			T.cell_fg[r][c] = T.cell_fg[r + 1][c];
			T.cell_bg[r][c] = T.cell_bg[r + 1][c];
		}
	}
	pane_erase_row_pen(bot);
	T.cur_row = bot;
	T.cur_col = 0;
	T.need_draw = 1;
	if (!T.dirty_full)
	{
		T.dirty_lo = top;
		T.dirty_hi = bot;
	}
}

static void pane_newline(void)
{
	/* #1227: DECSTBM only scrolls when the cursor sits on the bottom
	 * margin.  Below the region (or outside it in general) a line feed is
	 * ordinary cursor movement; at the pane bottom it clamps, with no
	 * scroll, matching DEC/xterm. */
	if (T.cur_row == T.scroll_bot)
	{
		pane_scroll_up();
		term_log_mark();
		return;
	}
	if (T.cur_row < T.pane_rows - 1)
	{
		T.cur_row++;
		T.cur_col = 0;
		mark_dirty_row(T.cur_row);
	}
	term_log_mark();
}

static void pane_put(char ch)
{
	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	if (T.cur_col >= term_width())
		pane_newline();
	if (T.cur_col < term_width() && T.cur_row < T.pane_rows)
	{
		term_plane_uncover_cell(T.cur_row, T.cur_col);
		T.cell[T.cur_row][T.cur_col] = ch;
		T.cell_fg[T.cur_row][T.cur_col] = term_pen();
		T.cell_bg[T.cur_row][T.cur_col] = term_paper();
		T.last_gch = (unsigned char)ch;
		T.cur_col++;
		mark_dirty_row(T.cur_row);
		term_log_mark();
	}
}

static void pane_puts(const char *s)
{
	const char *p;
	if (!s)
		return;
	for (p = s; *p; p++)
	{
		if (*p == '\r')
		{
			T.cur_col = 0;
			mark_dirty_row(T.cur_row);
		}
		else if (*p == '\n')
			pane_newline();
		else if ((unsigned char)*p >= 32)
			pane_put(*p);
	}
}

static void term_cell(int x, int y, unsigned ch, unsigned fg, unsigned bg)
{
	mmb_gfx_glyph_cell_topdown_h(x, y, ch, fg, bg, T.cell_h);
}

static void term_put_str_bg(int x, int y, const char *s, unsigned fg, unsigned bg)
{
	int i;
	if (!s)
		return;
	for (i = 0; s[i]; i++)
		term_cell(x + i * TM_CW, y, (unsigned char)s[i], fg, bg);
}

static void term_fill_cells(int x, int y, int n, unsigned bg)
{
	if (n > 0)
		mmb_gfx_fill_rect_topdown(x, y, n * TM_CW, T.cell_h, bg);
}

static void term_put_hot(int x, int y, const char *s, char hot, unsigned fg,
			 unsigned hot_fg, unsigned bg)
{
	int i, used = 0;

	if (!s)
		return;
	for (i = 0; s[i]; i++)
	{
		unsigned c_fg = fg;
		char ch = s[i];

		if (!used && (ch == hot || ch == hot + 32 || ch == hot - 32))
		{
			c_fg = hot_fg;
			used = 1;
		}
		term_cell(x + i * TM_CW, y, (unsigned char)ch, c_fg, bg);
	}
}

static void term_draw_status(void)
{
	char right[64];
	int y, n, x0;
	unsigned bg = TM_MENU_BG;
	unsigned fg = TM_MENU_FG;

	if (!T.menu && !T.alt_pend)
		return;
	y = term_status_y();
	x0 = 0;
	mmb_gfx_fill_rect_topdown(0, y, T.vid_cols * TM_CW, T.cell_h, bg);
	term_put_str_bg(x0, y, "Alt-X", TM_HOT, bg);
	term_put_str_bg(x0 + 5 * TM_CW, y, "  ", fg, bg);
	term_put_str_bg(x0 + 7 * TM_CW, y, "F10", TM_HOT, bg);
	if (T.demo)
	{
		term_cell(x0 + 13 * TM_CW, y, 176, fg, bg);
		term_cell(x0 + 14 * TM_CW, y, 177, fg, bg);
		term_cell(x0 + 15 * TM_CW, y, 178, fg, bg);
	}
	right[0] = 0;
	if (T.connecting)
	{
		if (mmb_net_tcp_cancelling())
			strncpy(right, "Cancelling...", sizeof(right) - 1);
		else
			strncpy(right, "Connecting...", sizeof(right) - 1);
	}
	else if (T.net_fail && T.net_msg[0])
		strncpy(right, T.net_msg, sizeof(right) - 1);
	else if (T.demo)
		strncpy(right, "demo", sizeof(right) - 1);
	else if (!T.host[0] && !T.tcp && !T.connecting)
		strncpy(right, "Disconnected", sizeof(right) - 1);
	else if (T.host[0])
		fmt_hostport(right, sizeof(right));
	{
		char echo[16];
		char mixed[80];
		strcpy(echo, term_want_echo() ? "Echo ON" : "Echo OFF");
		if (right[0])
		{
			strncpy(mixed, echo, sizeof(mixed) - 1);
			mixed[sizeof(mixed) - 1] = 0;
			n = (int)strlen(mixed);
			if (n + 2 < (int)sizeof(mixed))
			{
				mixed[n] = ' ';
				mixed[n + 1] = 0;
				strncat(mixed, right, sizeof(mixed) - 1 - (unsigned)n - 1);
			}
			strncpy(right, mixed, sizeof(right) - 1);
		}
		else
			strncpy(right, echo, sizeof(right) - 1);
	}
	right[sizeof(right) - 1] = 0;
	n = (int)strlen(right);
	if (n > T.vid_cols)
		n = T.vid_cols;
	term_put_str_bg((T.vid_cols - n) * TM_CW, y, right, fg, bg);
}

/* Drop-down row -> selectable item index, or -1 for a separator. */
static const signed char tm_menu_row_sel[TM_MENU_ROWS] = {
	0, 1, -1, 2, 3, 4, -1, 5
};

static void term_draw_menu(void)
{
	int x0, y0, w, i, row, wlen, drop_h, bar_w;
	const char *items[TM_MENU_ITEMS];
	unsigned brd = TM_DLG_FG;
	unsigned title_fg, title_bg;

	if (!T.menu && !T.alt_pend)
		return;
	items[0] = "Bookmarks";
	items[1] = "Downloads Folder";
	items[2] = term_width_label();
	items[3] = "Echo";
	items[4] = "Scrollback";
	items[5] = "Exit";
	w = 10;
	for (i = 0; i < TM_MENU_ITEMS; i++)
	{
		wlen = (int)strlen(items[i]);
		if (wlen + 2 > w)
			w = wlen + 2;
	}
	w += 2;
	bar_w = T.vid_cols * TM_CW;
	mmb_gfx_fill_rect_topdown(0, 0, bar_w, T.cell_h, TM_MENU_BG);
	title_fg = T.menu ? TM_SEL_FG : TM_MENU_FG;
	title_bg = T.menu ? TM_SEL_BG : TM_MENU_BG;
	term_cell(0, 0, ' ', title_fg, title_bg);
	term_put_hot(TM_CW, 0, "Terminal", 'T', title_fg, TM_HOT, title_bg);
	term_cell(9 * TM_CW, 0, ' ', title_fg, title_bg);
	if (!T.menu)
		return;
	x0 = TM_CW;
	y0 = T.cell_h;
	drop_h = TM_MENU_ROWS + 2;
	mmb_gfx_fill_rect_topdown(x0, y0, w * TM_CW, drop_h * T.cell_h, TM_DLG_BG);
	term_cell(x0, y0, TM_BOX_TL, brd, TM_DLG_BG);
	for (i = 1; i < w - 1; i++)
		term_cell(x0 + i * TM_CW, y0, TM_BOX_H, brd, TM_DLG_BG);
	term_cell(x0 + (w - 1) * TM_CW, y0, TM_BOX_TR, brd, TM_DLG_BG);
	for (row = 0; row < TM_MENU_ROWS; row++)
	{
		int y = y0 + (1 + row) * T.cell_h;
		int sel = tm_menu_row_sel[row];

		term_cell(x0, y, TM_BOX_V, brd, TM_DLG_BG);
		if (sel < 0)
		{
			for (i = 1; i < w - 1; i++)
				term_cell(x0 + i * TM_CW, y, TM_BOX_H, brd,
					  TM_DLG_BG);
		}
		else
		{
			unsigned fg = (sel == T.menu_sel) ? TM_SEL_FG
							 : TM_DLG_FG;
			unsigned bg = (sel == T.menu_sel) ? TM_SEL_BG
							 : TM_DLG_BG;

			term_fill_cells(x0 + TM_CW, y, w - 2, bg);
			term_cell(x0 + TM_CW, y, ' ', fg, bg);
			term_put_str_bg(x0 + 2 * TM_CW, y, items[sel], fg,
					bg);
			/* Echo and Scrollback carry their state as a
			 * right-aligned checkbox so the item text does not
			 * shift when it is toggled. */
			if (sel == 3 || sel == 4)
			{
				int on = (sel == 3)
						 ? term_want_echo()
						 : G.opt.term_scroll_buffer;

				term_put_str_bg(x0 + (w - 5) * TM_CW, y,
						on ? "[x]" : "[ ]", fg, bg);
			}
			term_cell(x0 + (w - 2) * TM_CW, y, ' ', fg, bg);
		}
		term_cell(x0 + (w - 1) * TM_CW, y, TM_BOX_V, brd, TM_DLG_BG);
	}
	{
		int y = y0 + (TM_MENU_ROWS + 1) * T.cell_h;
		term_cell(x0, y, TM_BOX_BL, brd, TM_DLG_BG);
		for (i = 1; i < w - 1; i++)
			term_cell(x0 + i * TM_CW, y, TM_BOX_H, brd, TM_DLG_BG);
		term_cell(x0 + (w - 1) * TM_CW, y, TM_BOX_BR, brd, TM_DLG_BG);
	}
	mmb_gfx_fill_rect_topdown(x0 + w * TM_CW, y0, 2 * TM_CW,
				  drop_h * T.cell_h, TM_SH_BG);
	mmb_gfx_fill_rect_topdown(x0 + 2 * TM_CW, y0 + drop_h * T.cell_h,
				  w * TM_CW, T.cell_h, TM_SH_BG);
}

static void term_shadow_blank(void)
{
	uint64_t blank = term_shadow_key(' ', TM_FG, TM_BG);
	int r, c, cols = term_width();

	for (r = 0; r < T.pane_rows && r < TM_MAX_ROWS; r++)
		for (c = 0; c < cols; c++)
			term_cell_sh[r][c] = blank;
}

static void term_draw_row(int r)
{
	int c, x, y;
	unsigned ch, fg, bg;
	uint64_t key;

	if (r < 0 || r >= T.pane_rows)
		return;
	y = T.pane_top + r * T.cell_h;
	c = 0;
	while (c < term_width())
	{
		/* A covered run composites the plane; adjacent cells are one
		 * scanline memcpy.  Page Up shows the text cells instead. */
		if (T.sb_view <= 0 && P_s[g_console] &&
		    P.cov[r * TM_MAX_COLS + c])
		{
			int c0 = c;

			while (c < term_width() && P.cov[r * TM_MAX_COLS + c])
				c++;
			term_plane_blit(r, c0, c);
			/* Force the glyph path when coverage clears later. */
			for (x = c0; x < c; x++)
				term_cell_sh[r][x] = 0;
			continue;
		}
		x = (T.pane_left + c) * TM_CW;
		term_view_cell(r, c, &ch, &fg, &bg);
		if (!ch)
			ch = ' ';
		if (T.sb_view <= 0 && term_paint_cursor && T.cur_vis &&
		    r == T.cur_row && c == T.cur_col)
		{
			unsigned t = fg;
			fg = bg;
			bg = t;
		}
		key = term_shadow_key(ch, fg, bg);
		if (key != term_cell_sh[r][c])
		{
			term_cell_sh[r][c] = key;
			term_cell(x, y, ch, fg, bg);
		}
		c++;
	}
}

static void term_rect_union(int *ax, int *ay, int *aw, int *ah,
			    int x, int y, int w, int h)
{
	int x2, y2, nx2, ny2;

	if (w <= 0 || h <= 0)
		return;
	if (*aw <= 0 || *ah <= 0)
	{
		*ax = x;
		*ay = y;
		*aw = w;
		*ah = h;
		return;
	}
	x2 = *ax + *aw;
	y2 = *ay + *ah;
	nx2 = x + w;
	ny2 = y + h;
	if (x < *ax)
		*ax = x;
	if (y < *ay)
		*ay = y;
	if (nx2 > x2)
		x2 = nx2;
	if (ny2 > y2)
		y2 = ny2;
	*aw = x2 - *ax;
	*ah = y2 - *ay;
}

static void term_present_async_rect(int x, int y, int w, int h)
{
	uint16_t *pg;
	int bw, bh;

	if (w <= 0 || h <= 0 || !G.plat)
		return;
	if (G.plat->term_present_async)
	{
		pg = mmb_gfx_buf_for(TM_PAGE, &bw, &bh);
		if (!pg)
			return;
		G.plat->term_present_async(x, y, w, h, pg + y * bw + x, bw);
		return;
	}
	/* Platforms without async DMA present through the generic path, which
	 * sources G.gfx.display_page. TERM paints into TM_PAGE but leaves
	 * display_page on the console page, so switch it for the present or the
	 * window shows the stale console page instead of the session. */
	{
		int saved_display = G.gfx.display_page;

		G.gfx.display_page = TM_PAGE;
		mmb_gfx_present_rect(x, y, w, h);
		G.gfx.display_page = saved_display;
	}
}

static void term_present_drain(void)
{
	if (G.plat && G.plat->term_present_drain)
		G.plat->term_present_drain();
}

/* A direct (unbounced) present DMAs straight out of PAGE 2, which TERM paints
 * into.  Drain before rewriting it so the in-flight source stays immutable
 * until completion (#330, #340). */
static void term_guard_present(void)
{
	if (G.plat && G.plat->term_present_locked && G.plat->term_present_locked())
		term_present_drain();
}

static void term_draw(void)
{
	int r, saved, lo, hi, full_screen;

	if (s_term_bg)
	{
		/* Background drain: the grid is updated and need_draw stays set, so
		 * returning to this console repaints from it (#858). */
		return;
	}
	if (!T.need_draw)
		return;
	if (T.cur_vis)
		mark_dirty_row(T.cur_row);
	if (T.cur_shown)
		mark_dirty_row(T.cur_shown_row);
	saved = G.gfx.write_page;
	G.gfx.write_page = TM_PAGE;
	G.gfx.display_page = 0;
	/* Present sources PAGE 2 directly, so a prior frame's DMA must finish
	 * before this repaint overwrites it (#340). Top up audio first: the
	 * drain below waits out that DMA. */
	mmb_play_mix();
	term_guard_present();
	full_screen = T.dirty_full;
	if (T.dirty_full)
	{
		mmb_gfx_cls(TM_BG);
		term_shadow_blank();
		lo = 0;
		hi = T.pane_rows - 1;
	}
	else
	{
		lo = T.dirty_lo;
		hi = T.dirty_hi;
	}
	if (lo >= 0 && hi >= lo)
	{
		for (r = lo; r <= hi; r++)
		{
			term_draw_row(r);
			if ((r & 3) == 0)
				term_yield();
		}
	}
	if (T.menu || T.alt_pend)
		term_draw_status();
	else if (T.sb_view > 0 || T.sb_search)
		term_draw_sb_status();
	else if (T.mode80x25)
	{
		if (T.pane_rows > 0)
			term_draw_row(T.pane_rows - 1);
	}
	else
		mmb_gfx_fill_rect_topdown(0, term_status_y(), T.vid_cols * TM_CW,
					  T.cell_h, TM_BG);
	term_draw_menu();
	term_draw_dlg();
	if (full_screen)
	{
		int fw, fh;

		/* Async coalesced present, same as dirty bands (#329), straight
		 * from the PAGE 2 paint buffer (#340). */
		(void)mmb_gfx_buf_for(TM_PAGE, &fw, &fh);
		term_present_async_rect(0, 0, fw, fh);
	}
	else
	{
		int px = 0, py = 0, pw = 0, ph = 0;
		int x0 = T.pane_left * TM_CW;
		int pane_w = term_width() * TM_CW;
		int pane_h = term_fb_h();

		if (T.menu || T.alt_pend)
		{
			int top_h = T.menu ? (TM_MENU_ROWS + 3) * T.cell_h : T.cell_h;

			term_rect_union(&px, &py, &pw, &ph, x0, 0, pane_w, pane_h);
			term_rect_union(&px, &py, &pw, &ph, 0, 0,
					T.vid_cols * TM_CW, top_h);
			term_rect_union(&px, &py, &pw, &ph, 0, term_status_y(),
					T.vid_cols * TM_CW, T.cell_h);
		}
		else if (T.sb_view > 0 || T.sb_search)
		{
			term_rect_union(&px, &py, &pw, &ph, 0, 0,
					T.vid_cols * TM_CW, pane_h);
			term_rect_union(&px, &py, &pw, &ph, 0, term_status_y(),
					T.vid_cols * TM_CW, T.cell_h);
		}
		else if (T.img_dirty)
		{
			int a, b;

			/* Present the image box, not the whole pane.  Text damage
			 * outside the image rows still needs its own band. */
			term_rect_union(&px, &py, &pw, &ph, T.img_x, T.img_y,
					T.img_w, T.img_h);
			if (lo >= 0 && hi >= lo)
			{
				a = lo;
				b = hi;
				if (a < T.img_lo)
				{
					int e = b < T.img_lo - 1 ? b : T.img_lo - 1;

					term_rect_union(&px, &py, &pw, &ph, x0,
							T.pane_top + a * T.cell_h,
							pane_w, (e - a + 1) * T.cell_h);
				}
				if (b > T.img_hi)
				{
					int s = a > T.img_hi + 1 ? a : T.img_hi + 1;

					term_rect_union(&px, &py, &pw, &ph, x0,
							T.pane_top + s * T.cell_h,
							pane_w, (b - s + 1) * T.cell_h);
				}
			}
			term_rect_union(&px, &py, &pw, &ph, 0, term_status_y(),
					T.vid_cols * TM_CW, T.cell_h);
		}
		else if (lo >= 0 && hi >= lo)
		{
			int y = T.pane_top + lo * T.cell_h;
			int rh = (hi - lo + 1) * T.cell_h;

			term_rect_union(&px, &py, &pw, &ph, x0, y, pane_w, rh);
			term_rect_union(&px, &py, &pw, &ph, 0, term_status_y(),
					T.vid_cols * TM_CW, T.cell_h);
		}
		else
			term_rect_union(&px, &py, &pw, &ph, x0, 0, pane_w, pane_h);
		if (pw > 0 && ph > 0)
			term_present_async_rect(px, py, pw, ph);
	}
	G.gfx.write_page = saved;
	T.need_draw = 0;
	T.dirty_full = 0;
	T.dirty_lo = -1;
	T.dirty_hi = -1;
	T.img_dirty = 0;
	T.cur_shown = T.cur_vis;
	T.cur_shown_row = T.cur_row;
}

static void term_exit(void)
{
	int bits;

	if (mmb_zm_active(&ZM))
		mmb_zm_cancel(&ZM);
	mmb_zm_forget(&ZM);
	zm_shift = 0;
	term_log_flush();
	mmb_net_tcp_close();
	mmb_tcp_release();
	T.tcp = 0;
	T.connecting = 0;
	term_present_drain();
	if (G.plat && G.plat->present_wait)
		G.plat->present_wait();
	G.gfx.write_page = 0;
	G.gfx.display_page = 0;
	bits = T.saved_bits;
	if (bits != 8 && bits != 12 && bits != 16 && bits != 32)
		bits = 16;
	if (T.saved_mode &&
	    (G.gfx.mode != T.saved_mode || G.gfx.bits != bits))
		mmb_gfx_set_mode(T.saved_mode, bits);
	else if (G.plat && G.plat->fill_screen)
		G.plat->fill_screen(0);
	mmb_console_reset_prompt();
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	mmb_net_rxbuf_reset(&term_rx);
	mmb_yield_remove(term_bg_yield);
	term_cache_free(g_console);
	term_apc_buf_free(g_console);
	term_pixbuf_free_all(g_console);
	term_mask_free(g_console);
	memset(&T, 0, sizeof(T));
	s_iac_n = 0;
	/* Keep the small log buffer so OPTION TERM LOG OFF can still report this
	 * session's counters; free the large scrollback ring on leave (#767). */
	term_sb_free(g_console);
	term_plane_free(g_console);
}

static void replay_tx(const unsigned char *p, unsigned n)
{
	static const char hexdig[] = "0123456789ABCDEF";
	char line[8 + TM_REPLAY_HEX + 4];
	unsigned i, o, chunk;

	while (n > 0)
	{
		chunk = n > (TM_REPLAY_HEX / 2) ? (TM_REPLAY_HEX / 2) : n;
		o = 0;
		line[o++] = '!';
		line[o++] = 'T';
		line[o++] = 'X';
		line[o++] = ' ';
		for (i = 0; i < chunk; i++)
		{
			line[o++] = hexdig[p[i] >> 4];
			line[o++] = hexdig[p[i] & 15];
		}
		line[o++] = '\r';
		line[o++] = '\n';
		line[o] = 0;
		ser(line);
		p += chunk;
		n -= chunk;
	}
}

static void term_net_send_raw(const void *data, unsigned n)
{
	const unsigned char *pb;

	if (!data || n == 0)
		return;
	pb = (const unsigned char *)data;
	if (pb[0] == IAC)
		term_log_tx(pb, (int)n);
	if (T.replay)
		replay_tx((const unsigned char *)data, n);
	else if (T.tcp && !T.file_replay)
	{
		const unsigned char *p = (const unsigned char *)data;
		int left = (int)n, rc, idle = 0;

		T.tx_n += n;
		term_yield();
		while (left > 0)
		{
			rc = mmb_net_tcp_send(p, (unsigned)left);
			if (rc == left)
				return;
			if (rc > 0)
			{
				p += rc;
				left -= rc;
				idle = 0;
				continue;
			}
			if (rc < 0)
				return;
			term_yield();
			if (T.tcp && !T.replay && !T.file_replay)
				term_tcp_drain(1);
			if (++idle > 500)
				return;
		}
	}
}

static void iac_flush(void)
{
	unsigned n;

	if (s_iac_n <= 0)
		return;
	n = (unsigned)s_iac_n;
	s_iac_n = 0;
	term_net_send_raw(s_iac_out, n);
}

static void iac_append(const unsigned char *p, unsigned n)
{
	if (!p || n == 0)
		return;
	if (s_iac_n + (int)n > (int)sizeof(s_iac_out))
		iac_flush();
	if (n > sizeof(s_iac_out))
	{
		iac_flush();
		term_net_send_raw(p, n);
		return;
	}
	memcpy(s_iac_out + s_iac_n, p, n);
	s_iac_n += (int)n;
}

static void term_net_send(const void *data, unsigned n)
{
	iac_flush();
	term_net_send_raw(data, n);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static void replay_flush_hex(void)
{
	unsigned char buf[TM_REPLAY_HEX / 2];
	int n = 0, i, hi, lo;

	if (T.replay_hex_n < 2)
	{
		T.replay_hex_n = 0;
		return;
	}
	for (i = 0; i + 1 < T.replay_hex_n && n < (int)sizeof(buf); i += 2)
	{
		hi = hexval(T.replay_hex[i]);
		lo = hexval(T.replay_hex[i + 1]);
		if (hi < 0 || lo < 0)
			break;
		buf[n++] = (unsigned char)((hi << 4) | lo);
	}
	T.replay_hex_n = 0;
	if (n > 0)
	{
		incoming_feed(buf, n);
		if (T.need_draw)
			term_draw();
		if (n < 80 || !T.dump_at ||
		    mmb_now_ms() - T.dump_at >= TM_DUMP_MS)
		{
			term_serial_dump();
			T.dump_at = mmb_now_ms();
		}
	}
}

static int replay_key(char c)
{
	unsigned char b = (unsigned char)c;

	if (T.replay_st == 0)
	{
		if (b == TM_RS)
		{
			T.replay_st = 1;
			T.replay_hex_n = 0;
			return 1;
		}
		return 0;
	}
	if (T.replay_st == 1)
	{
		if (c == 'R' || c == 'r')
		{
			T.replay_st = 2;
			return 1;
		}
		T.replay_st = 0;
		return 0;
	}
	if (T.replay_st == 2)
	{
		if (c == 'X' || c == 'x')
		{
			T.replay_st = 3;
			T.replay_hex_n = 0;
			return 1;
		}
		if (c == 'C' || c == 'c')
		{
			T.replay_st = 4;
			T.replay_hex_n = 0;
			return 1;
		}
		T.replay_st = 0;
		return 0;
	}
	if (T.replay_st == 4)
	{
		/* RS "RC" [reason] NL: the harness hangs up like a remote host */
		if (c == '\n' || c == '\r')
		{
			int i = 0;

			T.replay_st = 0;
			while (i < T.replay_hex_n && T.replay_hex[i] == ' ')
				i++;
			T.replay_hex[T.replay_hex_n] = 0;
			replay_closed(T.replay_hex + i);
			T.replay_hex_n = 0;
			return 1;
		}
		if (c >= ' ' && c < 127 && T.replay_hex_n + 1 < TM_REPLAY_HEX)
			T.replay_hex[T.replay_hex_n++] = c;
		return 1;
	}
	if (c == '\n' || c == '\r')
	{
		T.replay_st = 0;
		replay_flush_hex();
		return 1;
	}
	if (hexval(c) >= 0 && T.replay_hex_n < TM_REPLAY_HEX)
		T.replay_hex[T.replay_hex_n++] = c;
	return 1;
}

static void replay_mon(void)
{
	char line[96];
	int n = 0;

	if (!T.replay)
		return;
	if (T.mon_ansi == T.ansi_st && T.mon_iac == T.iac && T.mon_sb == T.sb)
		return;
	T.mon_ansi = T.ansi_st;
	T.mon_iac = T.iac;
	T.mon_sb = T.sb;
	line[n++] = '!';
	line[n++] = 'M';
	line[n++] = 'O';
	line[n++] = 'N';
	line[n++] = ' ';
	line[n++] = 'a';
	line[n++] = '=';
	line[n++] = (char)('0' + (T.ansi_st % 10));
	line[n++] = ' ';
	line[n++] = 'i';
	line[n++] = '=';
	line[n++] = (char)('0' + (T.iac % 10));
	line[n++] = ' ';
	line[n++] = 's';
	line[n++] = '=';
	line[n++] = (char)('0' + (T.sb ? 1 : 0));
	line[n++] = ' ';
	line[n++] = 'c';
	line[n++] = '=';
	line[n++] = (char)('0' + (T.char_mode ? 1 : 0));
	line[n++] = ' ';
	line[n++] = 'e';
	line[n++] = '=';
	line[n++] = (char)('0' + (term_want_echo() ? 1 : 0));
	line[n++] = '\r';
	line[n++] = '\n';
	line[n] = 0;
	ser(line);
}

static void send_iac(int cmd, int opt)
{
	unsigned char b[3];
	b[0] = IAC;
	b[1] = (unsigned char)cmd;
	b[2] = (unsigned char)opt;
	iac_append(b, 3);
}

static void send_ttype(void)
{
	static const unsigned char ttype[] = {
		IAC, SB, TELOPT_TTYPE, TTYPE_IS,
		'A', 'N', 'S', 'I',
		IAC, SE
	};
	iac_append(ttype, (unsigned)sizeof(ttype));
}

static void send_naws(void)
{
	unsigned char b[9];
	int cols = term_width();
	int rows = T.pane_rows > 0 ? T.pane_rows : 24;
	b[0] = IAC;
	b[1] = SB;
	b[2] = TELOPT_NAWS;
	b[3] = (unsigned char)((cols >> 8) & 255);
	b[4] = (unsigned char)(cols & 255);
	b[5] = (unsigned char)((rows >> 8) & 255);
	b[6] = (unsigned char)(rows & 255);
	b[7] = IAC;
	b[8] = SE;
	iac_append(b, 9);
}

static void telnet_announce(void)
{
	send_iac(WILL, TELOPT_TTYPE);
	send_iac(DO, TELOPT_SGA);
	send_iac(WILL, TELOPT_NAWS);
	send_naws();
	send_ttype();
	iac_flush();
}

static int term_want_echo(void)
{
	if (T.echo_user == 1)
		return 1;
	if (T.echo_user == 2)
		return 0;
	return !T.no_echo;
}

static void pane_rubout(void)
{
	if (T.cur_col > 0)
	{
		T.cur_col--;
		T.cell[T.cur_row][T.cur_col] = ' ';
		T.cell_fg[T.cur_row][T.cur_col] = TM_FG;
		T.cell_bg[T.cur_row][T.cur_col] = TM_BG;
		mark_dirty_row(T.cur_row);
		term_log_mark();
	}
}

static void term_echo_byte(unsigned char b)
{
	if (!term_want_echo())
		return;
	if (b == 8 || b == 127)
	{
		if (T.echo_typed > 0)
		{
			T.echo_typed--;
			pane_rubout();
		}
	}
	else if (b == '\r' || b == '\n')
	{
		T.echo_typed = 0;
		pane_newline();
	}
	else if (b >= 32)
	{
		T.echo_typed++;
		pane_put((char)b);
	}
}

static void term_echo_flush(void)
{
	if (T.need_draw)
		term_draw();
	term_serial_dump();
}

static void term_menu_move(int dir)
{
	T.menu_sel += dir;
	if (T.menu_sel < 0)
		T.menu_sel = TM_MENU_ITEMS - 1;
	if (T.menu_sel > TM_MENU_ITEMS - 1)
		T.menu_sel = 0;
	term_overlay_chrome();
}

static void term_toggle_echo(void)
{
	if (term_want_echo())
		T.echo_user = 2;
	else
		T.echo_user = 1;
	if (T.menu || T.alt_pend)
		term_overlay_chrome();
	else
	{
		mark_dirty_full();
		term_draw();
		term_serial_dump();
	}
}

static void term_toggle_scroll_buffer(void)
{
	G.opt.term_scroll_buffer = G.opt.term_scroll_buffer ? 0 : 1;
	if (!G.opt.term_scroll_buffer && T.sb_view > 0)
		sb_to_live();
	mmb_settings_save();
	if (T.menu || T.alt_pend)
		term_overlay_chrome();
	else
	{
		mark_dirty_full();
		term_draw();
		term_serial_dump();
	}
}

static void term_fill_pages(void)
{
	int saved = G.gfx.write_page;

	term_guard_present();
	G.gfx.write_page = TM_PAGE;
	mmb_gfx_cls(TM_BG);
	G.gfx.write_page = 0;
	mmb_gfx_cls(TM_BG);
	G.gfx.write_page = saved;
}

static void term_init_extra_cols(int old_cols)
{
	int r, c, cols = term_width();
	if (cols <= old_cols)
		return;
	for (r = 0; r < T.pane_rows; r++)
	{
		for (c = old_cols; c < cols; c++)
		{
			if (!T.cell[r][c])
			{
				T.cell[r][c] = ' ';
				T.cell_fg[r][c] = TM_FG;
				T.cell_bg[r][c] = TM_BG;
			}
		}
	}
}

static void term_clamp_cursor(void)
{
	if (T.cur_col >= term_width())
		T.cur_col = term_width() - 1;
	if (T.cur_col < 0)
		T.cur_col = 0;
	if (T.cur_row >= T.pane_rows)
		T.cur_row = T.pane_rows - 1;
	if (T.cur_row < 0)
		T.cur_row = 0;
}

static void term_apply_session_mode(void)
{
	int mode, bits;

	term_pick_cell_h();
	bits = T.saved_bits;
	if (bits != 8 && bits != 12 && bits != 16 && bits != 32)
		bits = 16;
	if (bits < 16)
		bits = 16;
	if (T.mode80x25 && T.cell_h == TM_CH_VGA)
		mode = TM_MODE_80X25;
	else if (T.mode80x25)
	{
		/* 80x25 on the 8x14 face: keep the display's own mode and let
		 * term_layout() centre the grid in the margin. */
		mode = T.saved_mode;
		if (!mmb_gfx_mode_valid(mode))
			mode = G.gfx.mode;
	}
	else if (T.letterbox && T.saved_mode != 19 && T.saved_mode != 20)
		mode = 14;
	else
	{
		/* #1200: the Chromebook's console runs in MODE 19/20 (1366x768 and
		 * 683x384), whose ~85x23 text grid is what TERM's boxed pane is
		 * designed around. Forcing MODE 14 there shrinks everything, so
		 * boxed TERM keeps those two modes; every other prompt mode still
		 * gets the 960x540 letterbox default. */
		mode = T.saved_mode;
		if (!mmb_gfx_mode_valid(mode))
			mode = 14;
	}
	if (G.gfx.mode != mode || G.gfx.bits != bits)
	{
		term_present_drain();
		mmb_gfx_set_mode(mode, bits);
	}
	G.gfx.write_page = TM_PAGE;
	G.gfx.display_page = 0;
}

/* Cycle the pane view Full -> Boxed -> 80x25 -> Full. 80x25 uses the same
 * session mode/font path as an 80x25 bookmark, including the 8x14 EGA
 * fallback when the display cannot fit 25 rows of 8x16 (#1155). */
static void term_cycle_view(void)
{
	int old_cols = term_width();

	if (T.mode80x25)
	{
		T.mode80x25 = 0;
		T.letterbox = 0;
	}
	else if (T.letterbox)
	{
		T.mode80x25 = 1;
		T.letterbox = 0;
	}
	else
		T.letterbox = 1;
	/* The plane tracks the pane size; drop it and let the next image
	 * allocate the new size (rescaling a live image is not worth it). */
	term_plane_free(g_console);
	T.img_dirty = 0;
	term_apply_session_mode();
	term_layout();
	term_reset_scroll_region();
	term_init_extra_cols(old_cols);
	term_clamp_cursor();
	term_fill_pages();
	if (T.tcp)
		send_naws();
	mark_dirty_full();
	term_draw();
	term_present_drain();
	term_serial_dump();
}

static void term_ui_refresh(void)
{
	mark_dirty_full();
	term_draw();
	term_serial_dump();
}

static void term_present_overlay(int extra_w, int extra_h)
{
	int x, y, pw, ph;

	if (dlg_cw <= 0 || dlg_ch <= 0)
		return;
	x = dlg_c0 * TM_CW;
	y = dlg_r0 * T.cell_h;
	pw = (dlg_cw + extra_w) * TM_CW;
	ph = (dlg_ch + extra_h) * T.cell_h;
	term_present_async_rect(x, y, pw, ph);
}

static void term_dlg_refresh(void)
{
	int saved;

	saved = G.gfx.write_page;
	G.gfx.write_page = TM_PAGE;
	term_guard_present();
	term_draw_dlg();
	term_present_overlay(2, 1);
	G.gfx.write_page = saved;
	term_serial_dump_dlg();
}

static void term_overlay_chrome(void)
{
	int saved;

	saved = G.gfx.write_page;
	G.gfx.write_page = TM_PAGE;
	term_guard_present();
	term_draw_status();
	term_draw_menu();
	{
		int px = 0, py = 0, pw = 0, ph = 0;

		term_rect_union(&px, &py, &pw, &ph, 0, 0, T.vid_cols * TM_CW,
				(TM_MENU_ROWS + 3) * T.cell_h);
		term_rect_union(&px, &py, &pw, &ph, 0, term_status_y(),
				T.vid_cols * TM_CW, T.cell_h);
		if (pw > 0 && ph > 0)
			term_present_async_rect(px, py, pw, ph);
	}
	G.gfx.write_page = saved;
	term_serial_dump();
}

static void term_open_menu(void)
{
	T.menu = 1;
	T.menu_sel = 0;
	T.alt_pend = 0;
	T.dlg = TM_DLG_NONE;
	term_ui_refresh();
}

static void term_close_menu(void)
{
	T.menu = 0;
	T.alt_pend = 0;
	mark_dirty_full();
	term_draw();
	term_serial_dump();
}

static int bm_name_cmp(const char *a, const char *b)
{
	while (*a && *b)
	{
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return (unsigned char)ca - (unsigned char)cb;
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static void bm_sort(void)
{
	int i, j;
	term_bm tmp;

	for (i = 1; i < g_bm_n; i++)
	{
		tmp = g_bm[i];
		j = i;
		while (j > 0 && bm_name_cmp(g_bm[j - 1].name, tmp.name) > 0)
		{
			g_bm[j] = g_bm[j - 1];
			j--;
		}
		g_bm[j] = tmp;
	}
}

static int bm_find_name(const char *name)
{
	int i;
	for (i = 0; i < g_bm_n; i++)
		if (bm_name_cmp(g_bm[i].name, name) == 0)
			return i;
	return 0;
}

static void bm_path(char *dst, unsigned n)
{
	/* C: on the live ISO is only a real persistent drive when MMCORE is
	 * mounted; otherwise keep the session's bookmarks on A: and migrate
	 * them back when the volume returns. */
	if (mmb_fat_ready('C') && !mmb_storage_persistence_lost())
		strncpy(dst, "C:/.termconfig", n - 1);
	else
		strncpy(dst, "A:/.termconfig", n - 1);
	dst[n - 1] = 0;
}

static void bm_append(char *buf, int sz, const char *s)
{
	int n = (int)strlen(buf);
	int i;
	for (i = 0; s && s[i] && n + 1 < sz; i++)
		buf[n++] = s[i];
	buf[n] = 0;
}

static void bm_append_int(char *buf, int sz, int v)
{
	char tmp[12];
	int i = 0, n;
	if (v < 0)
		v = 0;
	if (v == 0)
	{
		bm_append(buf, sz, "0");
		return;
	}
	n = v;
	while (n && i < 11)
	{
		tmp[i++] = (char)('0' + (n % 10));
		n /= 10;
	}
	while (i > 0)
	{
		char c[2];
		c[0] = tmp[--i];
		c[1] = 0;
		bm_append(buf, sz, c);
	}
}

static void term_dl_dirs_load(int keep_active);

static int s_bm_save_warned;

static void term_bm_save(void)
{
	char path[24];
	char buf[4096];
	int i;

	/* The active console's dir is authoritative while saving; other
	 * consoles' persisted values are refreshed from the file first so a
	 * Use on this console cannot drop them (#679). */
	term_dl_dirs_load(1);
	bm_sort();
	bm_path(path, sizeof(path));
	buf[0] = 0;
	bm_append(buf, sizeof(buf), "# TERM bookmarks\n");
	for (i = 0; i < g_bm_n; i++)
	{
		bm_append(buf, sizeof(buf), "\n[bm.");
		bm_append_int(buf, sizeof(buf), i);
		bm_append(buf, sizeof(buf), "]\nname=");
		bm_append(buf, sizeof(buf), g_bm[i].name);
		bm_append(buf, sizeof(buf), "\nhost=");
		bm_append(buf, sizeof(buf), g_bm[i].host);
		bm_append(buf, sizeof(buf), "\nport=");
		bm_append_int(buf, sizeof(buf), g_bm[i].port);
		bm_append(buf, sizeof(buf), "\necho=");
		bm_append_int(buf, sizeof(buf), g_bm[i].echo ? 1 : 0);
		bm_append(buf, sizeof(buf), "\nletterboxed=");
		bm_append_int(buf, sizeof(buf), g_bm[i].letterboxed ? 1 : 0);
		bm_append(buf, sizeof(buf), "\nmode80x25=");
		bm_append_int(buf, sizeof(buf), g_bm[i].mode80x25 ? 1 : 0);
		bm_append(buf, sizeof(buf), "\n");
	}
	for (i = 0; i < MMB_MAX_CONSOLES; i++)
	{
		if (!g_dl_dir_s[i][0])
			continue;
		bm_append(buf, sizeof(buf), "\ndownload_dir");
		bm_append_int(buf, sizeof(buf), i);
		bm_append(buf, sizeof(buf), "=");
		bm_append(buf, sizeof(buf), g_dl_dir_s[i]);
		bm_append(buf, sizeof(buf), "\n");
	}
	if (mmb_vfs_write(path, buf, (unsigned)strlen(buf), 0) != 0)
	{
		if (!s_bm_save_warned)
		{
			s_bm_save_warned = 1;
			mmb_storage_notice("!TERM bookmarks not saved: storage unavailable");
		}
	}
	else if (mmb_storage_persistence_lost())
	{
		if (!s_bm_save_warned)
		{
			s_bm_save_warned = 1;
			mmb_storage_notice("!TERM bookmarks are session-only: MMCORE not mounted");
		}
	}
	else
		s_bm_save_warned = 0;
}

static void bm_trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
	{
		memmove(s, s + 1, strlen(s));
	}
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = 0;
}

static int bm_parse_int(const char *s)
{
	int n = 0;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + (*s++ - '0');
	return n;
}

static void term_dl_store(char *dst, unsigned cap, const char *src)
{
	if (cap == 0)
		return;
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = 0;
}

/* Restore the persisted per-console download defaults (#679). "download_dirN"
 * sets console N and a legacy bare "download_dir" seeds any console that has
 * no per-console value yet. keep_active keeps this console's in-memory value
 * (used when saving) instead of letting the file overwrite it. */
static void term_dl_dirs_load(int keep_active)
{
	char path[24];
	char buf[4096];
	unsigned got = 0;
	char *p, *nl;

	bm_path(path, sizeof(path));
	if (!mmb_vfs_exists(path))
		return;
	if (mmb_vfs_read(path, buf, sizeof(buf) - 1, &got) != 0)
		return;
	buf[got] = 0;
	p = buf;
	while (*p)
	{
		char *eq;
		int i, idx = -1, legacy = 0;

		nl = p;
		while (*nl && *nl != '\n')
			nl++;
		if (*nl)
			*nl++ = 0;
		if (p[0] && p[strlen(p) - 1] == '\r')
			p[strlen(p) - 1] = 0;
		bm_trim(p);
		eq = p;
		while (*eq && *eq != '=')
			eq++;
		if (*eq == '=')
		{
			char *key = p;
			char *val = eq + 1;

			*eq = 0;
			bm_trim(key);
			bm_trim(val);
			if (mmb_keyword_eq(key, "download_dir"))
				legacy = 1;
			else
			{
				for (i = 0; i < MMB_MAX_CONSOLES; i++)
				{
					char want[16];

					strcpy(want, "download_dir");
					want[12] = (char)('0' + i);
					want[13] = 0;
					if (mmb_keyword_eq(key, want))
					{
						idx = i;
						break;
					}
				}
			}
			if (legacy)
			{
				for (i = 0; i < MMB_MAX_CONSOLES; i++)
				{
					if (keep_active && i == g_console)
						continue;
					if (!g_dl_dir_s[i][0])
						term_dl_store(g_dl_dir_s[i],
							sizeof(g_dl_dir_s[i]), val);
				}
			}
			else if (idx >= 0)
			{
				if (!(keep_active && idx == g_console))
					term_dl_store(g_dl_dir_s[idx],
						sizeof(g_dl_dir_s[idx]), val);
			}
		}
		p = nl;
	}
}

/* Parse one .termconfig into the in-memory list. Returns 0 when the file is
 * missing or cannot be read, leaving g_bm empty; the caller decides whether to
 * keep the previous list. */
static int term_bm_parse_file(const char *path)
{
	char buf[4096];
	unsigned got = 0;
	char *p, *nl;
	int cur = -1;

	g_bm_n = 0;
	memset(g_bm, 0, sizeof(g_bm));
	if (!mmb_vfs_exists(path))
		return 0;
	if (mmb_vfs_read(path, buf, sizeof(buf) - 1, &got) != 0)
		return 0;
	/* A cached stream left stale by a remount can report EOF for a file that
	 * still has bytes. Treat that as a failed read, not as "no bookmarks". */
	if (got == 0 && mmb_vfs_size(path) > 0)
		return 0;
	buf[got] = 0;
	p = buf;
	while (*p)
	{
		nl = p;
		while (*nl && *nl != '\n')
			nl++;
		if (*nl)
			*nl++ = 0;
		if (p[0] && p[strlen(p) - 1] == '\r')
			p[strlen(p) - 1] = 0;
		bm_trim(p);
		if (p[0] == 0 || p[0] == '#' || p[0] == ';')
		{
			p = nl;
			continue;
		}
		if (p[0] == '[')
		{
			if (p[1] == 'b' || p[1] == 'B')
			{
				if (g_bm_n < TM_BM_MAX)
				{
					cur = g_bm_n;
					memset(&g_bm[cur], 0, sizeof(g_bm[cur]));
					g_bm[cur].port = 23;
					g_bm[cur].echo = 1;
					g_bm[cur].letterboxed = 1;
					g_bm[cur].mode80x25 = 0;
					g_bm_n++;
				}
				else
					cur = -1;
			}
			else
				cur = -1;
			p = nl;
			continue;
		}
		if (cur < 0)
		{
			char *eq = p;
			while (*eq && *eq != '=')
				eq++;
			if (*eq == '=')
			{
				*eq++ = 0;
				bm_trim(p);
				bm_trim(eq);
			}
			p = nl;
			continue;
		}
		if (cur >= 0)
		{
			char *eq = p;
			while (*eq && *eq != '=')
				eq++;
			if (*eq == '=')
			{
				*eq++ = 0;
				bm_trim(p);
				bm_trim(eq);
				if (mmb_keyword_eq(p, "name"))
					strncpy(g_bm[cur].name, eq, TM_BM_NAME - 1);
				else if (mmb_keyword_eq(p, "host"))
					strncpy(g_bm[cur].host, eq, sizeof(g_bm[cur].host) - 1);
				else if (mmb_keyword_eq(p, "port"))
				{
					g_bm[cur].port = bm_parse_int(eq);
					if (g_bm[cur].port < 1 || g_bm[cur].port > 65535)
						g_bm[cur].port = 23;
				}
				else if (mmb_keyword_eq(p, "echo"))
					g_bm[cur].echo = bm_parse_int(eq) ? 1 : 0;
				else if (mmb_keyword_eq(p, "letterboxed"))
					g_bm[cur].letterboxed = bm_parse_int(eq) ? 1 : 0;
				else if (mmb_keyword_eq(p, "mode80x25"))
					g_bm[cur].mode80x25 = bm_parse_int(eq) ? 1 : 0;
			}
		}
		p = nl;
	}
	bm_sort();
	return 1;
}

static void term_bm_load(void)
{
	char path[24];
	const char *other;
	term_bm saved_list[TM_BM_MAX];
	int saved_n = g_bm_n;
	int loaded_from_a = 0;

	/* Opening Bookmarks reloads from disk. A read failure (for example the
	 * MMCORE mount is away right after a resume) must not look like "no
	 * bookmarks": keep the list already in memory instead. */
	memcpy(saved_list, g_bm, sizeof(saved_list));
	term_dl_dirs_load(0);
	bm_path(path, sizeof(path));
	/* Try the other drive too: while C: is volatile bm_path() prefers the
	 * session copy on A:, and after a recovery the older C: copy may be the
	 * only one with the user's bookmarks. */
	other = (path[0] == 'C') ? "A:/.termconfig" : "C:/.termconfig";
	if (!term_bm_parse_file(path))
	{
		if ((other[0] != 'C' || mmb_fat_ready('C')) &&
		    term_bm_parse_file(other))
			loaded_from_a = (other[0] == 'A');
		else
		{
			memcpy(g_bm, saved_list, sizeof(saved_list));
			g_bm_n = saved_n;
			bm_sort();
			return;
		}
	}
	/* A fallback read from A: is this session's live copy. Once C: is
	 * available again, write it there so it survives the next reboot. */
	if (loaded_from_a && mmb_fat_ready('C') &&
	    !mmb_storage_persistence_lost())
		term_bm_save();
}

static void dlg_center(int cw, int ch)
{
	int shw = 2, shh = 1;

	dlg_cw = cw;
	dlg_ch = ch;
	dlg_c0 = (T.vid_cols - cw) / 2;
	dlg_r0 = (T.vid_rows - ch) / 2;
	if (dlg_c0 < 0)
		dlg_c0 = 0;
	if (dlg_r0 < 0)
		dlg_r0 = 0;
	if (dlg_c0 + cw + shw > T.vid_cols && T.vid_cols > cw + shw)
		dlg_c0 = T.vid_cols - cw - shw;
	if (dlg_r0 + ch + shh > T.vid_rows && T.vid_rows > ch + shh)
		dlg_r0 = T.vid_rows - ch - shh;
	if (dlg_c0 < 0)
		dlg_c0 = 0;
	if (dlg_r0 < 0)
		dlg_r0 = 0;
}

static void term_dlg_hline_bg(int x0, int y0, int cw, unsigned left, unsigned mid,
			      unsigned right)
{
	int i;
	unsigned brd = TM_DLG_FG;

	term_cell(x0, y0, left, brd, TM_DLG_BG);
	for (i = 1; i < cw - 1; i++)
		term_cell(x0 + i * TM_CW, y0, mid, brd, TM_DLG_BG);
	term_cell(x0 + (cw - 1) * TM_CW, y0, right, brd, TM_DLG_BG);
}

static void term_dlg_frame(int cw, int ch, const char *title)
{
	int x0, y0, w, h, i, tw, tx;
	unsigned brd = TM_DLG_FG;

	dlg_center(cw, ch);
	x0 = dlg_c0 * TM_CW;
	y0 = dlg_r0 * T.cell_h;
	w = cw * TM_CW;
	h = ch * T.cell_h;
	mmb_gfx_fill_rect_topdown(x0, y0, w, h, TM_DLG_BG);
	term_dlg_hline_bg(x0, y0, cw, TM_BOX_TL, TM_BOX_H, TM_BOX_TR);
	term_dlg_hline_bg(x0, y0 + (ch - 1) * T.cell_h, cw, TM_BOX_BL, TM_BOX_H,
			 TM_BOX_BR);
	for (i = 1; i < ch - 1; i++)
	{
		term_cell(x0, y0 + i * T.cell_h, TM_BOX_V, brd, TM_DLG_BG);
		term_fill_cells(x0 + TM_CW, y0 + i * T.cell_h, cw - 2, TM_DLG_BG);
		term_cell(x0 + (cw - 1) * TM_CW, y0 + i * T.cell_h, TM_BOX_V, brd,
			  TM_DLG_BG);
	}
	mmb_gfx_fill_rect_topdown(x0 + w, y0 + T.cell_h, 2 * TM_CW, h - T.cell_h,
				  TM_SH_BG);
	mmb_gfx_fill_rect_topdown(x0 + TM_CW, y0 + h, w, T.cell_h, TM_SH_BG);
	if (title && title[0])
	{
		tw = (int)strlen(title);
		tx = dlg_c0 + (cw - tw) / 2;
		if (tx < dlg_c0 + 1)
			tx = dlg_c0 + 1;
		term_put_str_bg(tx * TM_CW, y0, title, brd, TM_DLG_BG);
	}
}

static void dlg_text_at(int dcol, int drow, const char *s, int hi)
{
	int x, y, n;
	unsigned fg, bg;

	if (!s)
		return;
	n = (int)strlen(s);
	x = (dlg_c0 + dcol) * TM_CW;
	y = (dlg_r0 + drow) * T.cell_h;
	fg = hi ? TM_SEL_FG : TM_DLG_FG;
	bg = hi ? TM_SEL_BG : TM_DLG_BG;
	if (n > 0)
		term_put_str_bg(x, y, s, fg, bg);
}

static char *dlg_focus_text(int *cap)
{
	if (T.dlg_focus == TM_ED_NAME)
	{
		if (cap)
			*cap = (int)sizeof(T.dlg_name);
		return T.dlg_name;
	}
	if (T.dlg_focus == TM_ED_HOST)
	{
		if (cap)
			*cap = (int)sizeof(T.dlg_host);
		return T.dlg_host;
	}
	if (cap)
		*cap = 0;
	return 0;
}

static void dlg_caret_home(void)
{
	int cap;
	char *s = dlg_focus_text(&cap);

	(void)cap;
	T.dlg_caret = s ? (int)strlen(s) : 0;
	T.dlg_off = 0;
}

static void dlg_field_scroll(int dcol, int drow, int width, const char *s, int hi,
			     int caret, int *off)
{
	int x, y, i, n, o;
	unsigned fg, bg;

	x = (dlg_c0 + dcol) * TM_CW;
	y = (dlg_r0 + drow) * T.cell_h;
	fg = hi ? TM_SEL_FG : TM_FIELD_FG;
	bg = hi ? TM_SEL_BG : TM_FIELD_BG;
	if (width < 1)
		width = 1;
	term_fill_cells(x, y, width, bg);
	s = s ? s : "";
	n = (int)strlen(s);
	o = off ? *off : 0;
	if (o < 0)
		o = 0;
	if (o > n)
		o = n;
	if (caret >= 0)
	{
		if (caret > n)
			caret = n;
		if (caret < o)
			o = caret;
		if (caret - o >= width)
			o = caret - width + 1;
	}
	if (o > n)
		o = n;
	if (o + width > n && o > 0)
	{
		o = n - width + 1;
		if (o < 0)
			o = 0;
	}
	if (off)
		*off = o;
	for (i = 0; i < width; i++)
	{
		int idx = o + i;
		if (caret == idx && hi && caret <= n)
			term_cell(x + i * TM_CW, y,
				  idx < n ? (unsigned char)s[idx] : ' ', bg, fg);
		else if (idx >= 0 && idx < n)
			term_cell(x + i * TM_CW, y, (unsigned char)s[idx], fg, bg);
	}
}

static void dlg_field(int dcol, int drow, int width, const char *s, int hi)
{
	int off = 0;

	dlg_field_scroll(dcol, drow, width, s, hi, -1, &off);
}

static void dlg_btn(int dcol, int drow, const char *s, int hi)
{
	char buf[24];
	int n;

	if (!s)
		return;
	n = (int)strlen(s);
	if (n > 20)
		n = 20;
	buf[0] = ' ';
	memcpy(buf + 1, s, (unsigned)n);
	buf[n + 1] = ' ';
	buf[n + 2] = 0;
	dlg_text_at(dcol, drow, buf, hi);
}

static void term_draw_dlg_list(void)
{
	int i, row;
	const char *btns[4];

	term_dlg_frame(48, 18, " Bookmarks ");
	if (T.dlg_sel < T.dlg_top)
		T.dlg_top = T.dlg_sel;
	if (T.dlg_sel >= T.dlg_top + TM_LIST_VIEW)
		T.dlg_top = T.dlg_sel - TM_LIST_VIEW + 1;
	if (T.dlg_top < 0)
		T.dlg_top = 0;
	if (g_bm_n == 0)
		dlg_text_at(2, 2, "(none)", 0);
	for (i = 0; i < TM_LIST_VIEW; i++)
	{
		int idx = T.dlg_top + i;
		char name[40];
		if (idx >= g_bm_n)
			break;
		row = 2 + i;
		strncpy(name, g_bm[idx].name[0] ? g_bm[idx].name : "(unnamed)",
			sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		dlg_field(2, row, 44, name, idx == T.dlg_sel);
	}
	btns[0] = "New";
	btns[1] = "Edit";
	btns[2] = "Delete";
	btns[3] = "Connect";
	dlg_btn(2, 16, btns[0], T.dlg_btn == 0);
	dlg_btn(8, 16, btns[1], T.dlg_btn == 1);
	dlg_btn(15, 16, btns[2], T.dlg_btn == 2);
	dlg_btn(36, 16, btns[3], T.dlg_btn == 3);
}

static void term_draw_dlg_edit(void)
{
	char port[8];
	char echo[16];
	char box[24];
	char m80[24];
	const char *save;

	term_dlg_frame(48, 15,
		       T.dlg_edit_idx < 0 ? " New bookmark " : " Edit bookmark ");
	dlg_text_at(2, 2, "Name", 0);
	dlg_field_scroll(8, 2, 36, T.dlg_name, T.dlg_focus == TM_ED_NAME,
			 T.dlg_focus == TM_ED_NAME ? T.dlg_caret : -1,
			 T.dlg_focus == TM_ED_NAME ? &T.dlg_off : 0);
	dlg_text_at(2, 4, "Host", 0);
	dlg_field_scroll(8, 4, 22, T.dlg_host, T.dlg_focus == TM_ED_HOST,
			 T.dlg_focus == TM_ED_HOST ? T.dlg_caret : -1,
			 T.dlg_focus == TM_ED_HOST ? &T.dlg_off : 0);
	dlg_text_at(32, 4, "Port", 0);
	port[0] = 0;
	bm_append_int(port, sizeof(port), T.dlg_port);
	dlg_field(38, 4, 6, port[0] ? port : "", T.dlg_focus == TM_ED_PORT);
	strcpy(echo, T.dlg_echo ? "[X] Echo ON" : "[ ] Echo ON");
	strcpy(box, T.dlg_letterbox ? "[X] Letterboxed" : "[ ] Letterboxed");
	strcpy(m80, T.dlg_mode80x25 ? "[X] Use 80x25 mode" : "[ ] Use 80x25 mode");
	dlg_text_at(2, 6, echo, T.dlg_focus == TM_ED_ECHO);
	dlg_text_at(2, 7, box, T.dlg_focus == TM_ED_BOX);
	dlg_text_at(2, 8, m80, T.dlg_focus == TM_ED_80X25);
	save = T.dlg_edit_idx < 0 ? "Save" : "Update";
	dlg_btn(2, 13, "Cancel", T.dlg_focus == TM_ED_CANCEL);
	dlg_btn(36, 13, save, T.dlg_focus == TM_ED_SAVE);
}

static void term_draw_dlg_del(void)
{
	const char *nm;

	term_dlg_frame(40, 7, " Delete bookmark? ");
	nm = (T.dlg_sel >= 0 && T.dlg_sel < g_bm_n && g_bm[T.dlg_sel].name[0])
		     ? g_bm[T.dlg_sel].name
		     : "";
	dlg_text_at(2, 2, nm, 0);
	dlg_btn(2, 5, "Yes", T.dlg_btn == 0);
	dlg_btn(28, 5, "No", T.dlg_btn == 1);
}

static void term_draw_dlg(void)
{
	if (T.dlg == TM_DLG_LIST)
		term_draw_dlg_list();
	else if (T.dlg == TM_DLG_EDIT)
		term_draw_dlg_edit();
	else if (T.dlg == TM_DLG_DEL)
		term_draw_dlg_del();
	else if (T.dlg == TM_DLG_DL)
		term_dl_draw();
}

static void term_serial_dump_dlg(void)
{
	int i;
	char port[8];

	if (T.live)
		return;
	if (T.dlg == TM_DLG_NONE)
		return;
	if (T.dlg == TM_DLG_LIST)
	{
		ser("Bookmarks\r\n");
		for (i = 0; i < g_bm_n; i++)
		{
			ser(g_bm[i].name);
			ser("\r\n");
		}
		ser("New\r\n");
		ser("Edit\r\n");
		ser("Delete\r\n");
		ser("Connect\r\n");
		return;
	}
	if (T.dlg == TM_DLG_EDIT)
	{
		ser(T.dlg_edit_idx < 0 ? "New bookmark\r\n" : "Edit bookmark\r\n");
		ser("Name\r\n");
		ser(T.dlg_name);
		ser("\r\nHost\r\n");
		ser(T.dlg_host);
		ser("\r\nPort\r\n");
		port[0] = 0;
		bm_append_int(port, sizeof(port), T.dlg_port);
		ser(port);
		ser("\r\n");
		ser(T.dlg_echo ? "[X] Echo ON\r\n" : "[ ] Echo ON\r\n");
		ser(T.dlg_letterbox ? "[X] Letterboxed\r\n" : "[ ] Letterboxed\r\n");
		ser(T.dlg_mode80x25 ? "[X] Use 80x25 mode\r\n" : "[ ] Use 80x25 mode\r\n");
		ser("Cancel\r\n");
		ser(T.dlg_edit_idx < 0 ? "Save\r\n" : "Update\r\n");
		return;
	}
	if (T.dlg == TM_DLG_DEL)
	{
		ser("Delete bookmark?\r\n");
		if (T.dlg_sel >= 0 && T.dlg_sel < g_bm_n)
		{
			ser(g_bm[T.dlg_sel].name);
			ser("\r\n");
		}
		ser("Yes\r\n");
		ser("No\r\n");
		return;
	}
	if (T.dlg == TM_DLG_DL)
		term_dl_serial();
}

static void term_bm_open_list(void)
{
	term_bm_load();
	T.menu = 0;
	T.dlg = TM_DLG_LIST;
	if (T.dlg_sel < 0 || T.dlg_sel >= g_bm_n)
		T.dlg_sel = 0;
	T.dlg_btn = TM_BTN_CONN;
	term_ui_refresh();
}

static void term_bm_open_edit(int is_new)
{
	if (!is_new && g_bm_n <= 0)
		return;
	T.dlg = TM_DLG_EDIT;
	T.dlg_focus = 0;
	if (is_new)
	{
		T.dlg_edit_idx = -1;
		T.dlg_name[0] = 0;
		T.dlg_host[0] = 0;
		T.dlg_port = 23;
		T.dlg_echo = 1;
		T.dlg_letterbox = 1;
		T.dlg_mode80x25 = 0;
	}
	else
	{
		term_bm *b = &g_bm[T.dlg_sel];
		T.dlg_edit_idx = T.dlg_sel;
		strncpy(T.dlg_name, b->name, sizeof(T.dlg_name) - 1);
		T.dlg_name[sizeof(T.dlg_name) - 1] = 0;
		strncpy(T.dlg_host, b->host, sizeof(T.dlg_host) - 1);
		T.dlg_host[sizeof(T.dlg_host) - 1] = 0;
		T.dlg_port = b->port;
		T.dlg_echo = b->echo ? 1 : 0;
		T.dlg_letterbox = b->letterboxed ? 1 : 0;
		T.dlg_mode80x25 = b->mode80x25 ? 1 : 0;
	}
	T.dlg_focus = TM_ED_NAME;
	dlg_caret_home();
	term_ui_refresh();
}

static void term_bm_save_edit(void)
{
	term_bm *b;
	char keep[TM_BM_NAME];

	if (!T.dlg_name[0] || !T.dlg_host[0])
		return;
	if (T.dlg_port < 1 || T.dlg_port > 65535)
		T.dlg_port = 23;
	if (T.dlg_edit_idx < 0)
	{
		if (g_bm_n >= TM_BM_MAX)
			return;
		b = &g_bm[g_bm_n++];
	}
	else
		b = &g_bm[T.dlg_edit_idx];
	memset(b, 0, sizeof(*b));
	strncpy(b->name, T.dlg_name, TM_BM_NAME - 1);
	strncpy(b->host, T.dlg_host, sizeof(b->host) - 1);
	b->port = T.dlg_port;
	b->echo = T.dlg_echo ? 1 : 0;
	b->letterboxed = T.dlg_letterbox ? 1 : 0;
	b->mode80x25 = T.dlg_mode80x25 ? 1 : 0;
	strncpy(keep, b->name, TM_BM_NAME - 1);
	keep[TM_BM_NAME - 1] = 0;
	term_bm_save();
	T.dlg_sel = bm_find_name(keep);
	T.dlg = TM_DLG_LIST;
	T.dlg_btn = TM_BTN_CONN;
	term_ui_refresh();
}

static void term_bm_delete(void)
{
	int i;
	if (T.dlg_sel < 0 || T.dlg_sel >= g_bm_n)
		return;
	for (i = T.dlg_sel; i + 1 < g_bm_n; i++)
		g_bm[i] = g_bm[i + 1];
	g_bm_n--;
	if (T.dlg_sel >= g_bm_n && g_bm_n > 0)
		T.dlg_sel = g_bm_n - 1;
	term_bm_save();
	T.dlg = TM_DLG_LIST;
	T.dlg_btn = TM_BTN_CONN;
	term_ui_refresh();
}

static void term_bm_connect(void)
{
	term_bm *b;
	int i, old_cols, n, use_socket;

	if (T.dlg_sel < 0 || T.dlg_sel >= g_bm_n)
		return;
	b = &g_bm[T.dlg_sel];
	if (!b->host[0])
		return;
	/* The TCP client is one machine resource, so a bookmark must not reset a
	 * connection another console holds (#778). Demo/replay bookmarks use no
	 * socket and must not close the owner's socket merely because the session
	 * is reset below. */
	use_socket = !(strcasecmp(b->host, "demo") == 0 ||
		       strcasecmp(b->host, "demoburst") == 0 ||
		       strcasecmp(b->host, "demoiac") == 0 ||
		       strcasecmp(b->host, "replay") == 0);
	if (use_socket && mmb_tcp_owner() >= 0 && mmb_tcp_owner() != g_console)
	{
		T.net_fail = 1;
		mmb_tcp_in_use_text(T.net_msg);
		pane_puts(T.net_msg);
		pane_newline();
		term_ui_refresh();
		return;
	}
	if (use_socket || mmb_tcp_owner() == g_console)
		tcp_close_quiet();
	T.connecting = 0;
	T.net_fail = 0;
	T.net_msg[0] = 0;
	T.live = 0;
	T.rx_n = 0;
	T.tx_n = 0;
	T.parsed_n = 0;
	T.char_mode = 0;
	T.no_echo = 0;
	T.echo_user = b->echo ? 1 : 2;
	T.iac = 0;
	T.iac_cmd = 0;
	T.sb = 0;
	T.linelen = 0;
	T.last_eol = 0;
	esc_reset();
	T.menu = 0;
	T.dlg = TM_DLG_NONE;
	T.cur_row = 0;
	T.cur_col = 0;
	T.cur_vis = 1;
	T.demo_line = 0;
	old_cols = term_width();
	strncpy(T.host, b->host, sizeof(T.host) - 1);
	T.host[sizeof(T.host) - 1] = 0;
	T.port = b->port;
	T.demo = host_is_demo();
	T.demo_burst = (strcasecmp(T.host, "demoburst") == 0);
	T.demo_iac = (strcasecmp(T.host, "demoiac") == 0);
	T.replay = host_is_replay();
	T.mode80x25 = b->mode80x25 ? 1 : 0;
	T.letterbox = T.mode80x25 ? 0 : (b->letterboxed ? 1 : 0);
	term_reset_pen();
	term_apply_session_mode();
	term_layout();
	term_reset_scroll_region();
	term_init_extra_cols(old_cols);
	term_clamp_cursor();
	for (i = 0; i < T.pane_rows; i++)
		pane_clear_row(i);
	term_fill_pages();
	if (T.replay)
	{
		T.tcp = 1;
		T.mon_ansi = -1;
		T.mon_iac = -1;
		T.mon_sb = -1;
		pane_puts("Connected");
		pane_newline();
	}
	else if (!T.demo)
	{
		if (mmb_tcp_any_open())
		{
			T.net_fail = 1;
			strncpy(T.net_msg, "TCP file open", sizeof(T.net_msg) - 1);
			pane_puts(T.net_msg);
			pane_newline();
		}
		else if (mmb_net_tcp_begin(T.host, T.port) != 0)
		{
			T.net_fail = 1;
			strncpy(T.net_msg, mmb_net_tcp_errmsg(), sizeof(T.net_msg) - 1);
			pane_puts(T.net_msg);
			pane_newline();
		}
		else
		{
			mmb_tcp_claim();
			term_mark_live();
			T.connecting = 1;
			T.connect_at = mmb_now_ms();
			pane_puts("Connecting...");
			pane_newline();
		}
	}
	else
	{
		if (!T.demo_iac)
		{
			n = T.demo_burst ? 40 : 2;
			T.demo_next = mmb_now_ms();
			while (n > 0 && T.demo_line <= 43)
			{
				demo_emit_line();
				n--;
			}
		}
	}
	if (T.tcp)
		telnet_announce();
	term_ui_refresh();
}

static void term_dlg_close(void)
{
	if (T.dlg == TM_DLG_EDIT || T.dlg == TM_DLG_DEL)
	{
		T.dlg = TM_DLG_LIST;
		T.dlg_btn = TM_BTN_CONN;
		term_ui_refresh();
		return;
	}
	T.dlg = TM_DLG_NONE;
	term_ui_refresh();
}

static void term_edit_add_char(char c)
{
	int cap, n;
	char *s;

	if (T.dlg_focus == TM_ED_PORT)
	{
		if (c >= '0' && c <= '9')
		{
			int v = T.dlg_port * 10 + (c - '0');
			if (T.dlg_port == 0 && c == '0')
				v = 0;
			if (v <= 65535)
				T.dlg_port = v;
		}
		return;
	}
	s = dlg_focus_text(&cap);
	if (!s || c < 32 || c >= 127)
		return;
	n = (int)strlen(s);
	if (n + 1 >= cap)
		return;
	if (T.dlg_caret < 0)
		T.dlg_caret = 0;
	if (T.dlg_caret > n)
		T.dlg_caret = n;
	memmove(s + T.dlg_caret + 1, s + T.dlg_caret,
		(size_t)(n - T.dlg_caret + 1));
	s[T.dlg_caret] = c;
	T.dlg_caret++;
}

static void term_edit_backspace(void)
{
	int cap, n;
	char *s;

	if (T.dlg_focus == TM_ED_PORT)
	{
		T.dlg_port /= 10;
		return;
	}
	s = dlg_focus_text(&cap);
	if (!s)
		return;
	(void)cap;
	n = (int)strlen(s);
	if (T.dlg_caret > n)
		T.dlg_caret = n;
	if (T.dlg_caret > 0)
	{
		memmove(s + T.dlg_caret - 1, s + T.dlg_caret,
			(size_t)(n - T.dlg_caret + 1));
		T.dlg_caret--;
	}
}

static void term_dlg_list_activate(void)
{
	if (T.dlg_btn == TM_BTN_NEW)
		term_bm_open_edit(1);
	else if (T.dlg_btn == TM_BTN_EDIT)
		term_bm_open_edit(0);
	else if (T.dlg_btn == TM_BTN_DEL)
	{
		if (g_bm_n <= 0)
			return;
		T.dlg = TM_DLG_DEL;
		T.dlg_btn = 0;
		term_ui_refresh();
	}
	else
		term_bm_connect();
}

static int term_dlg_key(char c)
{
	if (T.dlg == TM_DLG_DL)
		return term_dl_key(c);
	if (T.dlg == TM_DLG_LIST)
	{
		if (c == '\r' || c == '\n')
		{
			term_dlg_list_activate();
			return 1;
		}
		return 1;
	}
	if (T.dlg == TM_DLG_DEL)
	{
		if (c == '\r' || c == '\n')
		{
			if (T.dlg_btn == 0)
				term_bm_delete();
			else
			{
				T.dlg = TM_DLG_LIST;
				T.dlg_btn = TM_BTN_CONN;
				term_ui_refresh();
			}
		}
		if (c == 'y' || c == 'Y')
			term_bm_delete();
		if (c == 'n' || c == 'N')
		{
			T.dlg = TM_DLG_LIST;
			T.dlg_btn = TM_BTN_CONN;
			term_ui_refresh();
		}
		return 1;
	}
	if (T.dlg == TM_DLG_EDIT)
	{
		if (c == '\r' || c == '\n')
		{
			if (T.dlg_focus == TM_ED_ECHO)
			{
				T.dlg_echo = !T.dlg_echo;
				term_dlg_refresh();
				return 1;
			}
			if (T.dlg_focus == TM_ED_BOX)
			{
				T.dlg_letterbox = !T.dlg_letterbox;
				term_dlg_refresh();
				return 1;
			}
			if (T.dlg_focus == TM_ED_80X25)
			{
				T.dlg_mode80x25 = !T.dlg_mode80x25;
				term_dlg_refresh();
				return 1;
			}
			if (T.dlg_focus == TM_ED_CANCEL)
			{
				term_dlg_close();
				return 1;
			}
			term_bm_save_edit();
			return 1;
		}
		if (c == 8 || c == 127)
		{
			term_edit_backspace();
			term_dlg_refresh();
			return 1;
		}
		if (c == ' ' && (T.dlg_focus == TM_ED_ECHO || T.dlg_focus == TM_ED_BOX ||
				 T.dlg_focus == TM_ED_80X25))
		{
			if (T.dlg_focus == TM_ED_ECHO)
				T.dlg_echo = !T.dlg_echo;
			else if (T.dlg_focus == TM_ED_BOX)
				T.dlg_letterbox = !T.dlg_letterbox;
			else
				T.dlg_mode80x25 = !T.dlg_mode80x25;
			term_dlg_refresh();
			return 1;
		}
		if (c >= 32 && c < 127)
		{
			term_edit_add_char(c);
			term_dlg_refresh();
			return 1;
		}
		return 1;
	}
	return 0;
}

static void term_dlg_arrow(int c)
{
	if (T.dlg == TM_DLG_DL)
	{
		term_dl_arrow(c);
		return;
	}
	if (T.dlg == TM_DLG_LIST)
	{
		if (c == 'A' && g_bm_n > 0)
		{
			T.dlg_sel--;
			if (T.dlg_sel < 0)
				T.dlg_sel = g_bm_n - 1;
		}
		else if (c == 'B' && g_bm_n > 0)
		{
			T.dlg_sel++;
			if (T.dlg_sel >= g_bm_n)
				T.dlg_sel = 0;
		}
		else if (c == 'C')
		{
			T.dlg_btn++;
			if (T.dlg_btn > TM_BTN_CONN)
				T.dlg_btn = TM_BTN_NEW;
		}
		else if (c == 'D')
		{
			T.dlg_btn--;
			if (T.dlg_btn < TM_BTN_NEW)
				T.dlg_btn = TM_BTN_CONN;
		}
		term_dlg_refresh();
		return;
	}
	if (T.dlg == TM_DLG_DEL)
	{
		if (c == 'C' || c == 'D')
			T.dlg_btn = T.dlg_btn ? 0 : 1;
		term_dlg_refresh();
		return;
	}
	if (T.dlg == TM_DLG_EDIT)
	{
		if (c == 'A')
		{
			T.dlg_focus--;
			if (T.dlg_focus < 0)
				T.dlg_focus = TM_ED_SAVE;
			dlg_caret_home();
		}
		else if (c == 'B')
		{
			T.dlg_focus++;
			if (T.dlg_focus > TM_ED_SAVE)
				T.dlg_focus = 0;
			dlg_caret_home();
		}
		else if (c == 'C' || c == 'D')
		{
			if (T.dlg_focus == TM_ED_NAME || T.dlg_focus == TM_ED_HOST)
			{
				int cap, n;
				char *s = dlg_focus_text(&cap);
				(void)cap;
				n = (int)strlen(s);
				if (T.dlg_caret > n)
					T.dlg_caret = n;
				if (c == 'D' && T.dlg_caret > 0)
					T.dlg_caret--;
				else if (c == 'C' && T.dlg_caret < n)
					T.dlg_caret++;
				else if (T.dlg_focus == TM_ED_HOST)
				{
					T.dlg_focus = TM_ED_PORT;
					dlg_caret_home();
				}
			}
			else if (T.dlg_focus == TM_ED_PORT)
			{
				T.dlg_focus = TM_ED_HOST;
				dlg_caret_home();
			}
			else if (T.dlg_focus == TM_ED_CANCEL)
				T.dlg_focus = TM_ED_SAVE;
			else if (T.dlg_focus == TM_ED_SAVE)
				T.dlg_focus = TM_ED_CANCEL;
		}
		term_dlg_refresh();
	}
}

static void term_menu_activate(void)
{
	if (T.menu_sel == 0)
	{
		term_bm_open_list();
		return;
	}
	if (T.menu_sel == 1)
	{
		term_dl_open();
		return;
	}
	if (T.menu_sel == 2)
	{
		term_cycle_view();
		return;
	}
	if (T.menu_sel == 3)
	{
		term_toggle_echo();
		return;
	}
	if (T.menu_sel == 4)
	{
		term_toggle_scroll_buffer();
		return;
	}
	term_exit();
}

static void apply_option(int cmd, int opt)
{
	if (opt == TELOPT_ECHO)
	{
		T.echo_user = 0;
		if (cmd == WILL)
		{
			T.no_echo = 1;
			send_iac(DO, TELOPT_ECHO);
		}
		else if (cmd == WONT)
		{
			T.no_echo = 0;
			send_iac(DONT, TELOPT_ECHO);
		}
		else if (cmd == DO)
			send_iac(WONT, TELOPT_ECHO);
		else
			send_iac(WONT, TELOPT_ECHO);
		return;
	}
	if (opt == TELOPT_SGA)
	{
		if (cmd == WILL)
		{
			T.char_mode = 1;
			send_iac(DO, TELOPT_SGA);
		}
		else if (cmd == WONT)
		{
			T.char_mode = 0;
			send_iac(DONT, TELOPT_SGA);
		}
		else if (cmd == DO)
		{
			T.char_mode = 1;
			send_iac(WILL, TELOPT_SGA);
		}
		else
		{
			T.char_mode = 0;
			send_iac(WONT, TELOPT_SGA);
		}
		return;
	}
	if (opt == TELOPT_NAWS)
	{
		if (cmd == DO)
		{
			send_iac(WILL, TELOPT_NAWS);
			send_naws();
		}
		else if (cmd == WILL)
			send_iac(DONT, TELOPT_NAWS);
		else
			send_iac(WONT, TELOPT_NAWS);
		return;
	}
	if (opt == TELOPT_TTYPE)
	{
		if (cmd == DO)
		{
			send_iac(WILL, TELOPT_TTYPE);
			send_ttype();
		}
		else if (cmd == WILL)
			send_iac(DONT, TELOPT_TTYPE);
		else
			send_iac(WONT, TELOPT_TTYPE);
		return;
	}
	if (cmd == WILL)
		send_iac(DONT, opt);
	else if (cmd == DO)
		send_iac(WONT, opt);
}

static void ansi_cup(int row, int col)
{
	int old_r = T.cur_row;

	if (row < 1)
		row = 1;
	if (col < 1)
		col = 1;
	T.cur_row = row - 1;
	T.cur_col = col - 1;
	if (T.cur_row >= T.pane_rows)
		T.cur_row = T.pane_rows - 1;
	if (T.cur_col >= term_width())
		T.cur_col = term_width() - 1;
	if (T.cur_row < 0)
		T.cur_row = 0;
	if (T.cur_col < 0)
		T.cur_col = 0;
	mark_dirty_row(old_r);
	mark_dirty_row(T.cur_row);
}

static void ansi_sgr(void)
{
	int i, n, v;
	n = T.ansi_narg > 0 ? T.ansi_narg : 1;
	if (T.ansi_narg == 0)
		T.ansi_arg[0] = 0;
	for (i = 0; i < n; i++)
	{
		v = T.ansi_arg[i];
		if (v == 0)
			term_reset_pen();
		else if (v == 1)
			T.ansi_bold = 1;
		else if (v == 5 || v == 6)
		{
			T.ansi_ice = 1;
			ansi_update_bg();
		}
		else if (v == 7)
			T.ansi_inv = 1;
		else if (v == 22)
			T.ansi_bold = 0;
		else if (v == 25)
		{
			T.ansi_ice = 0;
			ansi_update_bg();
		}
		else if (v == 27)
			T.ansi_inv = 0;
		else if (v >= 30 && v <= 37)
			T.cur_fg = ansi_pal(v - 30);
		else if (v >= 90 && v <= 97)
			T.cur_fg = ansi_pal(v - 90 + 8);
		else if (v >= 40 && v <= 47)
		{
			T.ansi_bg_base = v - 40;
			ansi_update_bg();
		}
		else if (v >= 100 && v <= 107)
		{
			T.ansi_bg_base = v - 100 + 8;
			ansi_update_bg();
		}
		else if (v == 39)
			T.cur_fg = TM_FG;
		else if (v == 49)
		{
			T.ansi_bg_base = 0;
			ansi_update_bg();
		}
		else if ((v == 38 || v == 48) && i + 4 < n && T.ansi_arg[i + 1] == 2)
		{
			unsigned c = mmb_rgb_pack(T.ansi_arg[i + 2] & 255,
						  T.ansi_arg[i + 3] & 255,
						  T.ansi_arg[i + 4] & 255);
			if (v == 38)
				T.cur_fg = c;
			else
			{
				T.ansi_bg_base = -1;
				T.cur_bg = c;
			}
			i += 4;
		}
		else if ((v == 38 || v == 48) && i + 2 < n && T.ansi_arg[i + 1] == 5)
		{
			unsigned c = ansi_256(T.ansi_arg[i + 2]);
			if (v == 38)
				T.cur_fg = c;
			else
			{
				T.ansi_bg_base = -1;
				T.cur_bg = c;
			}
			i += 2;
		}
	}
}

static void ansi_reply(const char *s)
{
	if (T.tcp && s)
		term_net_send(s, (unsigned)strlen(s));
}

static void ansi_dsr(void)
{
	char buf[24];
	int n = 0, r = T.cur_row + 1, c = T.cur_col + 1;
	buf[n++] = 27;
	buf[n++] = '[';
	if (r >= 10)
		buf[n++] = (char)('0' + r / 10);
	buf[n++] = (char)('0' + r % 10);
	buf[n++] = ';';
	if (c >= 100)
		buf[n++] = (char)('0' + c / 100);
	if (c >= 10)
		buf[n++] = (char)('0' + (c / 10) % 10);
	buf[n++] = (char)('0' + c % 10);
	buf[n++] = 'R';
	buf[n] = 0;
	ansi_reply(buf);
}

static int ansi_arg(int i, int dflt)
{
	if (i < T.ansi_narg && T.ansi_arg[i] > 0)
		return T.ansi_arg[i];
	return dflt;
}

static char *ansi_put_uint(char *p, unsigned v)
{
	char tmp[12];
	int n = 0;

	if (!v)
	{
		*p++ = '0';
		return p;
	}
	while (v)
	{
		tmp[n++] = (char)('0' + v % 10u);
		v /= 10u;
	}
	while (n > 0)
		*p++ = tmp[--n];
	return p;
}

/* XTWINOPS report: CSI kind ; height ; width t */
static void ansi_reply_xtwin(int kind, int height, int width)
{
	char buf[40];
	char *p = buf;

	*p++ = 27;
	*p++ = '[';
	p = ansi_put_uint(p, (unsigned)kind);
	*p++ = ';';
	p = ansi_put_uint(p, (unsigned)height);
	*p++ = ';';
	p = ansi_put_uint(p, (unsigned)width);
	*p++ = 't';
	*p = 0;
	ansi_reply(buf);
}

/* XTSMGRAPHICS reply: CSI ? Pi ; status ; a [; b] S */
static void ansi_reply_xtsm(int pi, int status, int a, int b, int two)
{
	char buf[48];
	char *p = buf;

	*p++ = 27;
	*p++ = '[';
	*p++ = '?';
	p = ansi_put_uint(p, (unsigned)pi);
	*p++ = ';';
	p = ansi_put_uint(p, (unsigned)status);
	*p++ = ';';
	p = ansi_put_uint(p, (unsigned)a);
	if (two)
	{
		*p++ = ';';
		p = ansi_put_uint(p, (unsigned)b);
	}
	*p++ = 'S';
	*p = 0;
	ansi_reply(buf);
}

static void ansi_exec_xtsm(void)
{
	int pi = T.ansi_narg > 0 ? T.ansi_arg[0] : 0;
	int pa = T.ansi_narg > 1 ? T.ansi_arg[1] : 0;
	int w = term_width() * TM_CW;
	int h = T.pane_rows * T.cell_h;

	if (pi == 1)
	{
		if (pa == 1 || pa == 2 || pa == 4)
			ansi_reply_xtsm(1, 0, 256, 0, 0);
		else if (pa == 3)
			ansi_reply_xtsm(1, 3, 0, 0, 0);
		else
			ansi_reply_xtsm(1, 2, 0, 0, 0);
	}
	else if (pi == 2)
	{
		if (pa == 1 || pa == 2 || pa == 4)
			ansi_reply_xtsm(2, 0, w, h, 1);
		else if (pa == 3)
			ansi_reply_xtsm(2, 3, 0, 0, 0);
		else
			ansi_reply_xtsm(2, 2, 0, 0, 0);
	}
	else
		ansi_reply_xtsm(pi, 1, 0, 0, 0);
}

static void ansi_erase_line(int mode)
{
	int c, a, b;
	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	a = 0;
	b = term_width();
	if (mode == 0)
		a = T.cur_col;
	else if (mode == 1)
		b = T.cur_col + 1;
	for (c = a; c < b; c++)
	{
		T.cell[T.cur_row][c] = ' ';
		T.cell_fg[T.cur_row][c] = term_pen();
		T.cell_bg[T.cur_row][c] = term_paper();
	}
	term_plane_uncover_range(T.cur_row, a, b);
	mark_dirty_row(T.cur_row);
}

static void ansi_erase_disp(int mode)
{
	int r, a, b, c, w = term_width();
	unsigned fg = term_pen();
	unsigned bg = term_paper();

	a = 0;
	b = T.pane_rows;
	if (mode == 0)
		a = T.cur_row;
	else if (mode == 1)
		b = T.cur_row + 1;
	for (r = a; r < b && r < T.pane_rows; r++)
	{
		int ca = (mode == 0 && r == T.cur_row) ? T.cur_col : 0;
		int cb = (mode == 1 && r == T.cur_row) ? T.cur_col + 1 : w;

		if (ca < 0)
			ca = 0;
		if (cb > w)
			cb = w;
		for (c = ca; c < cb; c++)
		{
			T.cell[r][c] = ' ';
			T.cell_fg[r][c] = fg;
			T.cell_bg[r][c] = bg;
		}
		term_plane_uncover_range(r, ca, cb);
		mark_dirty_row(r);
	}
	if (mode == 2 || mode == 3)
	{
		T.cur_row = 0;
		T.cur_col = 0;
		term_plane_clear();
		mark_dirty_full();
		return;
	}
}

static void ansi_blank_cell(int r, int c)
{
	term_plane_uncover_cell(r, c);
	T.cell[r][c] = ' ';
	T.cell_fg[r][c] = term_pen();
	T.cell_bg[r][c] = term_paper();
}

/* IL (CSI n L): insert n blank lines at the cursor row, pushing the lines
 * below down and off the bottom margin.  The cursor is unaffected.  Inserted
 * lines carry the active SGR background (BCE), like ED/EL/ECH.  Bounded by the
 * DECSTBM bottom margin, and a no-op when the cursor is below it. (#1223) */
static void ansi_insert_lines(int n)
{
	int r, c, w = term_width(), bot = T.scroll_bot;

	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	if (T.cur_row > bot)
		return;
	if (n < 1)
		n = 1;
	if (n > bot - T.cur_row + 1)
		n = bot - T.cur_row + 1;
	term_plane_scroll_rows(T.cur_row, bot, n, 0);
	for (r = bot; r >= T.cur_row + n; r--)
		for (c = 0; c < w; c++)
		{
			T.cell[r][c] = T.cell[r - n][c];
			T.cell_fg[r][c] = T.cell_fg[r - n][c];
			T.cell_bg[r][c] = T.cell_bg[r - n][c];
		}
	for (r = T.cur_row; r < T.cur_row + n; r++)
		pane_erase_row_pen(r);
	for (r = T.cur_row; r <= bot; r++)
		mark_dirty_row(r);
}

/* DL (CSI n M): delete n lines from the cursor row, pulling the lines below
 * up and blanking the freed rows at the bottom margin (BCE).  Cursor
 * unaffected; bounded by the DECSTBM bottom margin. */
static void ansi_delete_lines(int n)
{
	int r, c, w = term_width(), bot = T.scroll_bot;

	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	if (T.cur_row > bot)
		return;
	if (n < 1)
		n = 1;
	if (n > bot - T.cur_row + 1)
		n = bot - T.cur_row + 1;
	term_plane_scroll_rows(T.cur_row, bot, n, 1);
	for (r = T.cur_row; r <= bot - n; r++)
		for (c = 0; c < w; c++)
		{
			T.cell[r][c] = T.cell[r + n][c];
			T.cell_fg[r][c] = T.cell_fg[r + n][c];
			T.cell_bg[r][c] = T.cell_bg[r + n][c];
		}
	for (r = bot - n + 1; r <= bot; r++)
		pane_erase_row_pen(r);
	for (r = T.cur_row; r <= bot; r++)
		mark_dirty_row(r);
}

/* ICH (CSI n @): insert n blank cells at the cursor, shifting the rest of the
 * row right and discarding what leaves the right edge.  Inserted cells carry
 * the active SGR background (BCE).  The cursor is unaffected. */
static void ansi_insert_chars(int n)
{
	int c, w = term_width();

	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	if (n < 1)
		n = 1;
	if (T.cur_col < 0)
		T.cur_col = 0;
	if (T.cur_col >= w || n > w - T.cur_col)
		n = w - T.cur_col;
	if (n <= 0)
		return;
	term_plane_shift_cols(T.cur_row, T.cur_col, n, 1);
	for (c = w - 1; c >= T.cur_col + n; c--)
	{
		T.cell[T.cur_row][c] = T.cell[T.cur_row][c - n];
		T.cell_fg[T.cur_row][c] = T.cell_fg[T.cur_row][c - n];
		T.cell_bg[T.cur_row][c] = T.cell_bg[T.cur_row][c - n];
	}
	for (c = T.cur_col; c < T.cur_col + n; c++)
		ansi_blank_cell(T.cur_row, c);
	mark_dirty_row(T.cur_row);
}

/* DCH (CSI n P): delete n cells at the cursor, shifting the rest of the row
 * left and blanking the freed cells at the right edge (BCE). */
static void ansi_delete_chars(int n)
{
	int c, w = term_width();

	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	if (n < 1)
		n = 1;
	if (T.cur_col < 0)
		T.cur_col = 0;
	if (T.cur_col >= w || n > w - T.cur_col)
		n = w - T.cur_col;
	if (n <= 0)
		return;
	term_plane_shift_cols(T.cur_row, T.cur_col, n, 0);
	for (c = T.cur_col; c < w - n; c++)
	{
		T.cell[T.cur_row][c] = T.cell[T.cur_row][c + n];
		T.cell_fg[T.cur_row][c] = T.cell_fg[T.cur_row][c + n];
		T.cell_bg[T.cur_row][c] = T.cell_bg[T.cur_row][c + n];
	}
	for (c = w - n; c < w; c++)
		ansi_blank_cell(T.cur_row, c);
	mark_dirty_row(T.cur_row);
}

/* REP (CSI n b): repeat the last graphic character n times, with the active
 * SGR pen.  A no-op until something graphic has been printed. */
static void ansi_repeat(int n)
{
	int i;

	if (n < 1)
		n = 1;
	if (!T.last_gch)
		return;
	for (i = 0; i < n; i++)
		pane_put((char)T.last_gch);
}

/* SU (CSI n S) / SD (CSI n T): scroll the DECSTBM region up/down n lines.
 * With the default region this is the whole pane.  Unlike the newline scroll
 * the cursor stays put and nothing is added to scrollback. (#1223, #1225) */
static void ansi_scroll_pane(int n, int up)
{
	int r, c, w = term_width();
	int top = T.scroll_top, bot = T.scroll_bot, rows = bot - top + 1;

	if (rows < 1)
		return;
	if (n < 1)
		n = 1;
	if (n > rows)
		n = rows;
	term_plane_scroll_rows(top, bot, n, up);
	if (up)
	{
		for (r = top; r <= bot - n; r++)
			for (c = 0; c < w; c++)
			{
				T.cell[r][c] = T.cell[r + n][c];
				T.cell_fg[r][c] = T.cell_fg[r + n][c];
				T.cell_bg[r][c] = T.cell_bg[r + n][c];
			}
		for (r = bot - n + 1; r <= bot; r++)
			pane_erase_row_pen(r);
	}
	else
	{
		for (r = bot; r >= top + n; r--)
			for (c = 0; c < w; c++)
			{
				T.cell[r][c] = T.cell[r - n][c];
				T.cell_fg[r][c] = T.cell_fg[r - n][c];
				T.cell_bg[r][c] = T.cell_bg[r - n][c];
			}
		for (r = top; r < top + n; r++)
			pane_erase_row_pen(r);
	}
	for (r = 0; r < T.pane_rows; r++)
		mark_dirty_row(r);
}

/* DECSTBM (CSI top;bot r): set the scrolling region, clamped to the pane.
 * Omitted or invalid parameters (top >= bot) reset the region to the whole
 * pane, matching xterm; the cursor moves home afterwards. (#1225) */
static void ansi_set_scroll_region(void)
{
	int rows = T.pane_rows;
	int top, bot;

	if (rows < 1)
		return;
	top = ansi_arg(0, 1);
	bot = ansi_arg(1, rows);
	if (top < 1)
		top = 1;
	if (bot < 1 || bot > rows)
		bot = rows;
	if (top >= bot)
		term_reset_scroll_region();
	else
	{
		T.scroll_top = top - 1;
		T.scroll_bot = bot - 1;
	}
	ansi_cup(1, 1);
}

static void ansi_exec_csi(char cmd)
{
	int n = ansi_arg(0, 1);
	int old_r = T.cur_row;
	if (cmd == 'A')
	{
		T.cur_row -= n;
		if (T.cur_row < 0)
			T.cur_row = 0;
		mark_dirty_row(old_r);
		mark_dirty_row(T.cur_row);
	}
	else if (cmd == 'B')
	{
		T.cur_row += n;
		if (T.cur_row >= T.pane_rows)
			T.cur_row = T.pane_rows - 1;
		mark_dirty_row(old_r);
		mark_dirty_row(T.cur_row);
	}
	else if (cmd == 'C')
	{
		T.cur_col += n;
		if (T.cur_col >= term_width())
			T.cur_col = term_width() - 1;
		mark_dirty_row(T.cur_row);
	}
	else if (cmd == 'D')
	{
		T.cur_col -= n;
		if (T.cur_col < 0)
			T.cur_col = 0;
		mark_dirty_row(T.cur_row);
	}
	else if (cmd == 'G')
		ansi_cup(T.cur_row + 1, ansi_arg(0, 1));
	else if (cmd == 'd')
		ansi_cup(ansi_arg(0, 1), T.cur_col + 1);
	else if (cmd == 'X')
	{
		int c, cnt = n < 1 ? 1 : n;
		int w = term_width();
		if (T.cur_row >= 0 && T.cur_row < T.pane_rows)
		{
			for (c = T.cur_col; c < T.cur_col + cnt && c < w; c++)
			{
				T.cell[T.cur_row][c] = ' ';
				T.cell_fg[T.cur_row][c] = term_pen();
				T.cell_bg[T.cur_row][c] = term_paper();
			}
			mark_dirty_row(T.cur_row);
		}
	}
	else if (cmd == 'H' || cmd == 'f')
		ansi_cup(ansi_arg(0, 1), ansi_arg(1, 1));
	else if (cmd == 'L')
		ansi_insert_lines(n);
	else if (cmd == 'M')
		ansi_delete_lines(n);
	else if (cmd == '@')
		ansi_insert_chars(n);
	else if (cmd == 'P')
		ansi_delete_chars(n);
	else if (cmd == 'b')
		ansi_repeat(n);
	else if (cmd == 'S')
		ansi_scroll_pane(n, 1);
	else if (cmd == 'T')
		ansi_scroll_pane(n, 0);
	else if (cmd == 'r')
		ansi_set_scroll_region();
	else if (cmd == 'J')
		ansi_erase_disp(T.ansi_narg ? T.ansi_arg[0] : 0);
	else if (cmd == 'K')
		ansi_erase_line(T.ansi_narg ? T.ansi_arg[0] : 0);
	else if (cmd == 'm')
		ansi_sgr();
	else if (cmd == 's')
	{
		T.sav_row = T.cur_row;
		T.sav_col = T.cur_col;
	}
	else if (cmd == 'u')
		ansi_cup(T.sav_row + 1, T.sav_col + 1);
	else if (cmd == 'n')
	{
		if (ansi_arg(0, 0) == 6)
			ansi_dsr();
		else if (ansi_arg(0, 0) == 5)
			ansi_reply("\x1b[0n");
	}
	else if (cmd == 'c')
	{
		if (G.opt.term_cterm)
			ansi_reply("\x1b[=67;84;101;114;109;1;332c");
		else
			ansi_reply("\x1b[?1;2;4c");
	}
	else if (cmd == 't')
	{
		int q = T.ansi_narg > 0 ? T.ansi_arg[0] : 0;
		int h = T.pane_rows * T.cell_h;
		int w = term_width() * TM_CW;

		if (q == 14)
			ansi_reply_xtwin(4, h, w);
		else if (q == 16)
			ansi_reply_xtwin(6, T.cell_h, TM_CW);
		else if (q == 18)
			ansi_reply_xtwin(8, T.pane_rows, term_width());
		else if (q == 11)
			ansi_reply("\x1b[1t");
	}
}

static void ansi_exec_priv(char cmd)
{
	int i, n, v;

	if (cmd == 'c' && T.ansi_priv == 2)
	{
		ansi_reply("\x1b[<0;4c");
		return;
	}
	if (cmd == 'S')
	{
		ansi_exec_xtsm();
		return;
	}
	if (cmd != 'h' && cmd != 'l')
		return;
	n = T.ansi_narg > 0 ? T.ansi_narg : 1;
	if (T.ansi_narg == 0)
		T.ansi_arg[0] = 0;
	for (i = 0; i < n; i++)
	{
		v = T.ansi_arg[i];
		if (v == 25)
			term_set_cursor_vis(cmd == 'h');
		else if (v == 80)
			T.sixel_scroll = (cmd == 'l');
	}
}

/* ---- SyncTERM APC cache, decoder and draw --------------------------- */

typedef struct {
	uint32_t a, b, c, d;
	uint64_t len;
	unsigned char buf[64];
} tm_md5;

static uint32_t tm_md5_rol(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void tm_md5_block(tm_md5 *m, const unsigned char *p)
{
	static const uint32_t K[64] = {
		0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
		0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
		0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
		0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
		0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
		0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
		0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
		0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
		0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
		0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
		0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
		0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
		0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
		0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
		0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
		0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
	};
	static const int S[64] = {
		7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
	};
	uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
		       ((uint32_t)p[i * 4 + 2] << 16) |
		       ((uint32_t)p[i * 4 + 3] << 24);
	for (i = 0; i < 64; i++)
	{
		uint32_t f, g, t;

		if (i < 16)
		{
			f = (b & c) | (~b & d);
			g = (uint32_t)i;
		}
		else if (i < 32)
		{
			f = (d & b) | (~d & c);
			g = (uint32_t)(5 * i + 1) & 15u;
		}
		else if (i < 48)
		{
			f = b ^ c ^ d;
			g = (uint32_t)(3 * i + 5) & 15u;
		}
		else
		{
			f = c ^ (b | ~d);
			g = (uint32_t)(7 * i) & 15u;
		}
		t = d;
		d = c;
		c = b;
		b = b + tm_md5_rol(a + f + K[i] + w[g], S[i]);
		a = t;
	}
	m->a += a;
	m->b += b;
	m->c += c;
	m->d += d;
}

static void tm_md5_init(tm_md5 *m)
{
	m->a = 0x67452301u;
	m->b = 0xefcdab89u;
	m->c = 0x98badcfeu;
	m->d = 0x10325476u;
	m->len = 0;
}

static void tm_md5_update(tm_md5 *m, const void *data, unsigned n)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned have = (unsigned)(m->len & 63u);
	unsigned i;

	m->len += n;
	if (have)
	{
		i = 64u - have;
		if (i > n)
			i = n;
		memcpy(m->buf + have, p, i);
		if (have + i < 64)
			return;
		tm_md5_block(m, m->buf);
		p += i;
		n -= i;
	}
	while (n >= 64)
	{
		tm_md5_block(m, p);
		p += 64;
		n -= 64;
	}
	if (n)
		memcpy(m->buf, p, n);
}

static void tm_md5_final(tm_md5 *m, unsigned char out[16])
{
	static const unsigned char pad[64] = { 0x80 };
	uint64_t bits = m->len * 8u;
	unsigned char tail[8];
	unsigned have = (unsigned)(m->len & 63u);
	unsigned padn = have < 56 ? 56 - have : 120 - have;
	int i;

	for (i = 0; i < 8; i++)
		tail[i] = (unsigned char)(bits >> (i * 8));
	tm_md5_update(m, pad, padn);
	tm_md5_update(m, tail, 8);
	for (i = 0; i < 4; i++)
	{
		out[i] = (unsigned char)(m->a >> (i * 8));
		out[i + 4] = (unsigned char)(m->b >> (i * 8));
		out[i + 8] = (unsigned char)(m->c >> (i * 8));
		out[i + 12] = (unsigned char)(m->d >> (i * 8));
	}
}

static void tm_md5_hex(const unsigned char digest[16], char out[33])
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 0; i < 16; i++)
	{
		out[i * 2] = hex[digest[i] >> 4];
		out[i * 2 + 1] = hex[digest[i] & 15];
	}
	out[32] = 0;
}

static int term_safe_name(const char *s)
{
	size_t i, n;

	if (!s)
		return 0;
	n = strlen(s);
	if (n < 1 || n >= TM_CACHE_NAME || s[0] == '.')
		return 0;
	for (i = 0; i < n; i++)
	{
		char c = s[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' ||
		      c == '-'))
			return 0;
		if (c == '.' && i + 1 < n && s[i + 1] == '.')
			return 0;
	}
	return 1;
}

static int term_cache_ensure(void)
{
	if (TC_s[g_console])
		return 0;
	if (!G.plat || !G.plat->alloc)
		return -1;
	TC_s[g_console] = (term_cache *)G.plat->alloc(sizeof(term_cache));
	if (!TC_s[g_console])
		return -1;
	memset(TC_s[g_console], 0, sizeof(term_cache));
	return 0;
}

static void term_cache_drop(term_cache_ent *e)
{
	if (e->data && G.plat && G.plat->free)
		G.plat->free(e->data);
	e->data = NULL;
	e->len = 0;
	e->live = 0;
	e->name[0] = 0;
}

static void term_cache_free(int idx)
{
	term_cache *c = TC_s[idx];
	int i;

	if (!c)
		return;
	for (i = 0; i < TM_CACHE_ENTRIES; i++)
		if (c->e[i].live)
			term_cache_drop(&c->e[i]);
	if (G.plat && G.plat->free)
		G.plat->free(c);
	TC_s[idx] = 0;
}

static term_cache_ent *term_cache_find(const char *name)
{
	int i;

	if (!TC_s[g_console])
		return NULL;
	for (i = 0; i < TM_CACHE_ENTRIES; i++)
	{
		if (TC.e[i].live && strcmp(TC.e[i].name, name) == 0)
		{
			TC.e[i].used = ++TC.tick;
			return &TC.e[i];
		}
	}
	return NULL;
}

static void term_cache_evict(void)
{
	term_cache_ent *victim = NULL;
	int i;

	for (i = 0; i < TM_CACHE_ENTRIES; i++)
	{
		term_cache_ent *e = &TC.e[i];

		if (!e->live)
			continue;
		if (!victim || e->used < victim->used)
			victim = e;
	}
	if (!victim)
		return;
	TC.total -= victim->len;
	term_cache_drop(victim);
}

static int term_cache_store(const char *name, const unsigned char *data,
			    unsigned len)
{
	term_cache_ent *e = NULL;
	tm_md5 md5;
	int i;

	if (!term_safe_name(name) || len > TM_CACHE_FILE_MAX ||
	    term_cache_ensure() != 0)
		return -1;
	for (i = 0; i < TM_CACHE_ENTRIES; i++)
	{
		if (TC.e[i].live && strcmp(TC.e[i].name, name) == 0)
		{
			TC.total -= TC.e[i].len;
			term_cache_drop(&TC.e[i]);
			break;
		}
	}
	while (TC.total + len > TM_CACHE_TOTAL_MAX)
	{
		unsigned before = TC.total;

		term_cache_evict();
		if (TC.total == before)
			return -1;
	}
	for (i = 0; i < TM_CACHE_ENTRIES; i++)
	{
		if (!TC.e[i].live)
		{
			e = &TC.e[i];
			break;
		}
	}
	while (!e)
	{
		unsigned before = TC.total;

		term_cache_evict();
		if (TC.total == before)
			return -1;
		for (i = 0; i < TM_CACHE_ENTRIES; i++)
			if (!TC.e[i].live)
			{
				e = &TC.e[i];
				break;
			}
	}
	if (len)
	{
		e->data = (unsigned char *)G.plat->alloc(len);
		if (!e->data)
			return -1;
		memcpy(e->data, data, len);
	}
	else
		e->data = NULL;
	e->len = len;
	e->live = 1;
	e->used = ++TC.tick;
	strncpy(e->name, name, TM_CACHE_NAME - 1);
	e->name[TM_CACHE_NAME - 1] = 0;
	tm_md5_init(&md5);
	tm_md5_update(&md5, data, len);
	tm_md5_final(&md5, e->md5);
	TC.total += len;
	return 0;
}

static int term_glob_match(const char *pat, const char *s)
{
	const char *star = NULL, *ss = s;

	while (*s)
	{
		if (*pat == '?' || *pat == *s)
		{
			pat++;
			s++;
		}
		else if (*pat == '*')
		{
			star = pat++;
			ss = s;
		}
		else if (star)
		{
			pat = star + 1;
			s = ++ss;
		}
		else
			return 0;
	}
	while (*pat == '*')
		pat++;
	return *pat == 0;
}

static void term_apc_list(const char *pat)
{
	char line[TM_CACHE_NAME + 40];
	char hex[33];
	int i;

	ansi_reply("\x1b_SyncTERM:C;L\n");
	if (!TC_s[g_console] || !pat)
	{
		ansi_reply("\x1b\\");
		return;
	}
	for (i = 0; i < TM_CACHE_ENTRIES; i++)
	{
		term_cache_ent *e = &TC.e[i];
		size_t n;

		if (!e->live || !term_glob_match(pat, e->name))
			continue;
		tm_md5_hex(e->md5, hex);
		n = strlen(e->name);
		memcpy(line, e->name, n);
		line[n++] = '\t';
		memcpy(line + n, hex, 32);
		n += 32;
		line[n++] = '\n';
		line[n] = 0;
		ansi_reply(line);
	}
	ansi_reply("\x1b\\");
}

static int tm_b64_val(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static int tm_b64_decode(const char *s, unsigned n, unsigned max_out,
			 unsigned char **out, unsigned *outlen)
{
	unsigned i, chars = 0, pad = 0, o = 0, acc = 0, bits = 0;
	unsigned char *buf;
	int seen_pad = 0;

	for (i = 0; i < n; i++)
	{
		unsigned char c = (unsigned char)s[i];

		if (c == '\r' || c == '\n')
			continue;
		if (c == '=')
		{
			seen_pad = 1;
			pad++;
			if (pad > 2)
				return -1;
			continue;
		}
		if (seen_pad || tm_b64_val(c) < 0)
			return -1;
		chars++;
	}
	if (chars == 0 || (chars & 3u) == 1u || ((chars + pad) & 3u) != 0)
		return -1;
	i = (chars + pad) / 4u * 3u - pad;
	if (i > max_out)
		return -1;
	buf = (unsigned char *)malloc(i ? i : 1);
	if (!buf)
		return -1;
	for (i = 0; i < n; i++)
	{
		unsigned char c = (unsigned char)s[i];
		int v;

		if (c == '\r' || c == '\n')
			continue;
		v = tm_b64_val(c);
		if (v < 0)
			break;
		acc = (acc << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			buf[o++] = (unsigned char)((acc >> bits) & 255u);
		}
	}
	*out = buf;
	*outlen = o;
	return 0;
}

static int tm_opt_int(const char *s, size_t n, int *out, int neg)
{
	size_t i = 0;
	long v = 0;
	int sign = 1;

	if (n == 0)
		return -1;
	if (s[0] == '-')
	{
		if (!neg)
			return -1;
		sign = -1;
		i = 1;
	}
	if (i >= n)
		return -1;
	for (; i < n; i++)
	{
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (s[i] - '0');
		if (v > 1048576L)
			return -1;
	}
	*out = (int)(v * sign);
	return 0;
}

static int tm_draw_key(const char *t, size_t n, term_draw_opts *o)
{
	const char *v;
	size_t vn;

	if (n == 2 && t[0] == 'F' && (t[1] == 'X' || t[1] == 'Y'))
	{
		if (t[1] == 'X')
			o->fx = 1;
		else
			o->fy = 1;
		return 1;
	}
	if (n == 4 && memcmp(t, "MBUF", 4) == 0)
	{
		o->mask_kind = TM_MASK_BUF;
		return 1;
	}
	if (n >= 6 && memcmp(t, "MFILE=", 6) == 0)
	{
		v = t + 6;
		vn = n - 6;
		if (vn < 1 || vn >= TM_CACHE_NAME)
			return -1;
		memcpy(o->mfile, v, vn);
		o->mfile[vn] = 0;
		if (!term_safe_name(o->mfile))
			return -1;
		o->mask_kind = TM_MASK_FILE;
		return 1;
	}
	if (n >= 5 && memcmp(t, "MASK=", 5) == 0)
	{
		o->mask_ptr = t + 5;
		o->mask_len = (unsigned)(n - 5);
		o->mask_kind = TM_MASK_INLINE;
		return 1;
	}
	if (n >= 3 && t[0] == 'B' && t[1] == '=')
		return tm_opt_int(t + 2, n - 2, &o->buf, 0) == 0 ? 1 : -1;
	if (n < 4 || t[2] != '=')
		return 0;
	v = t + 3;
	vn = n - 3;
	if (t[0] == 'S' && t[1] == 'X')
		return tm_opt_int(v, vn, &o->sx, 0) == 0 ? 1 : -1;
	if (t[0] == 'S' && t[1] == 'Y')
		return tm_opt_int(v, vn, &o->sy, 0) == 0 ? 1 : -1;
	if (t[0] == 'S' && t[1] == 'W')
		return tm_opt_int(v, vn, &o->sw, 0) == 0 ? 1 : -1;
	if (t[0] == 'S' && t[1] == 'H')
		return tm_opt_int(v, vn, &o->sh, 0) == 0 ? 1 : -1;
	if (t[0] == 'D' && t[1] == 'X')
		return tm_opt_int(v, vn, &o->dx, 1) == 0 ? 1 : -1;
	if (t[0] == 'D' && t[1] == 'Y')
		return tm_opt_int(v, vn, &o->dy, 1) == 0 ? 1 : -1;
	if (t[0] == 'Z' && t[1] == 'X')
		return tm_opt_int(v, vn, &o->zx, 0) == 0 ? 1 : -1;
	if (t[0] == 'Z' && t[1] == 'Y')
		return tm_opt_int(v, vn, &o->zy, 0) == 0 ? 1 : -1;
	if (t[0] == 'M' && t[1] == 'X')
		return tm_opt_int(v, vn, &o->mx, 0) == 0 ? 1 : -1;
	if (t[0] == 'M' && t[1] == 'Y')
		return tm_opt_int(v, vn, &o->my, 0) == 0 ? 1 : -1;
	if (t[0] == 'M' && t[1] == 'W')
		return tm_opt_int(v, vn, &o->mw, 0) == 0 ? 1 : -1;
	if (t[0] == 'M' && t[1] == 'H')
		return tm_opt_int(v, vn, &o->mh, 0) == 0 ? 1 : -1;
	return 0;
}

static const char *tm_draw_payload(const char *body, unsigned len,
				   const char *p, term_draw_opts *o)
{
	(void)body;
	(void)len;
	while (*p == ';')
	{
		const char *t = p + 1;
		const char *e = strchr(t, ';');
		size_t n = e ? (size_t)(e - t) : strlen(t);
		int k = tm_draw_key(t, n, o);

		if (k < 0)
			return NULL;
		if (k == 0)
			return t;
		if (!e)
			return NULL;
		p = e;
	}
	return NULL;
}

static void term_plane_prime_cells(int x0, int y0, int x1, int y1)
{
	uint16_t *page;
	int bw, bh, c0, c1, r0, r1, r, c;
	int saved;

	if (!P_s[g_console])
		return;
	page = mmb_gfx_buf_for(TM_PAGE, &bw, &bh);
	if (!page)
		return;
	c0 = x0 / TM_CW;
	c1 = (x1 - 1) / TM_CW;
	r0 = y0 / T.cell_h;
	r1 = (y1 - 1) / T.cell_h;
	if (c0 < 0)
		c0 = 0;
	if (r0 < 0)
		r0 = 0;
	if (c1 >= term_width())
		c1 = term_width() - 1;
	if (r1 >= T.pane_rows)
		r1 = T.pane_rows - 1;
	saved = G.gfx.write_page;
	G.gfx.write_page = TM_PAGE;
	for (r = r0; r <= r1; r++)
	{
		for (c = c0; c <= c1; c++)
		{
			unsigned ch, fg, bg;
			int px = (T.pane_left + c) * TM_CW;
			int py = T.pane_top + r * T.cell_h;
			int dy;

			if (P.cov[r * TM_MAX_COLS + c])
				continue;
			term_view_cell(r, c, &ch, &fg, &bg);
			term_cell(px, py, ch ? ch : ' ', fg, bg);
			for (dy = 0; dy < T.cell_h; dy++)
			{
				int plane_y = r * T.cell_h + dy;

				if (py + dy >= bh || plane_y >= P.h ||
				    px + TM_CW > bw || c * TM_CW + TM_CW > P.w)
					break;
				memcpy(P.pix + plane_y * P.stride + c * TM_CW,
				       page + (py + dy) * bw + px,
				       TM_CW * sizeof(uint16_t));
			}
		}
	}
	G.gfx.write_page = saved;
}

/* Decode a PBM (P1/P4) from the cache into the tightly packed bit layout the
 * SyncTERM mask sampler uses: black pixels are "draw". */
static int term_mask_from_pbm(const unsigned char *data, unsigned len,
			      term_mask *m)
{
	mmb_img_rgb img;
	unsigned char *bits;
	size_t n, bytes, i;
	int x, y, w, h;

	memset(&img, 0, sizeof(img));
	if (!data || len < 2 || data[0] != 'P' ||
	    (data[1] != '1' && data[1] != '4'))
		return -1;
	if (mmb_ppm_decode(data, len, TM_MASK_MAX_PIXELS, &img) != 0)
		return -1;
	w = img.w;
	h = img.h;
	n = (size_t)w * (size_t)h;
	bytes = (n + 7u) / 8u;
	bits = (unsigned char *)malloc(bytes ? bytes : 1);
	if (!bits)
	{
		mmb_img_free(&img);
		return -1;
	}
	memset(bits, 0, bytes ? bytes : 1);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			const unsigned char *p = img.rgb +
				((size_t)y * (size_t)w + (size_t)x) * 3u;

			if (p[0] == 0 && p[1] == 0 && p[2] == 0)
			{
				i = (size_t)y * (size_t)w + (size_t)x;
				bits[i >> 3] |= (unsigned char)(0x80u >> (i & 7u));
			}
		}
	mmb_img_free(&img);
	m->bits = bits;
	m->w = w;
	m->h = h;
	m->owned = 1;
	return 0;
}

/* Resolve the Draw/Paste mask tokens.  sw/sh are the effective source
 * rectangle, needed to default MW/MH and to bound-check MFILE/MBUF.  A
 * requested mask that does not validate drops the whole frame, matching
 * SyncTERM (it does not fall back to an unmasked draw). */
static int term_mask_resolve(const term_draw_opts *o, int sw, int sh,
			     term_mask *m)
{
	memset(m, 0, sizeof(*m));
	if (o->mask_kind == TM_MASK_INLINE)
	{
		unsigned char *buf = NULL;
		unsigned outlen = 0;
		unsigned long long px;
		size_t need;
		int mw, mh;

		mw = o->mw > 0 ? o->mw : sw;
		mh = o->mh > 0 ? o->mh : sh;
		if (mw < 1 || mh < 1)
			return -1;
		px = (unsigned long long)(unsigned)mw * (unsigned)mh;
		if (px > TM_MASK_MAX_PIXELS)
			return -1;
		need = (size_t)((px + 7u) / 8u);
		if (tm_b64_decode(o->mask_ptr, o->mask_len, TM_APC_MAX,
				  &buf, &outlen) != 0)
			return -1;
		if (outlen < need)
		{
			free(buf);
			return -1;
		}
		if (outlen > need)
		{
			/* SyncTERM accepts padding past the packed bitmap; keep
			 * only the bytes the sampler can address. */
			unsigned char *trim = (unsigned char *)malloc(need);

			if (!trim)
			{
				free(buf);
				return -1;
			}
			memcpy(trim, buf, need);
			free(buf);
			buf = trim;
		}
		m->bits = buf;
		m->w = mw;
		m->h = mh;
		m->owned = 1;
	}
	else if (o->mask_kind == TM_MASK_FILE)
	{
		term_cache_ent *e = term_cache_find(o->mfile);

		if (!e || term_mask_from_pbm(e->data, e->len, m) != 0)
			return -1;
	}
	else if (o->mask_kind == TM_MASK_BUF)
	{
		term_mask *src = &TMASK_s[g_console];

		if (!src->bits)
			return -1;
		*m = *src;
		m->owned = 0;
	}
	else
		return -1;

	if (o->mx < 0 || o->my < 0 ||
	    (long long)o->mx + sw > m->w ||
	    (long long)o->my + sh > m->h)
	{
		if (m->owned)
			free(m->bits);
		memset(m, 0, sizeof(*m));
		return -1;
	}
	return 0;
}

static void term_draw_image(const mmb_img_rgb *img, const term_draw_opts *o)
{
	int sx, sy, sw, sh, zx, zy, x, y;
	long long dx, dy, x0, x1, y0, y1;
	term_mask mask;
	int have_mask = 0;

	if (!img || !img->rgb || img->w < 1 || img->h < 1)
		return;
	sx = o->sx;
	sy = o->sy;
	sw = o->sw;
	sh = o->sh;
	zx = o->zx > 0 ? o->zx : 1;
	zy = o->zy > 0 ? o->zy : 1;
	if (sx < 0 || sy < 0 || sx >= img->w || sy >= img->h)
		return;
	if (sw <= 0)
		sw = img->w - sx;
	if (sh <= 0)
		sh = img->h - sy;
	if (sw > img->w - sx)
		sw = img->w - sx;
	if (sh > img->h - sy)
		sh = img->h - sy;
	if (sw < 1 || sh < 1)
		return;
	if (o->mask_kind != TM_MASK_NONE)
	{
		if (term_mask_resolve(o, sw, sh, &mask) != 0)
			return;
		have_mask = 1;
	}
	if (term_plane_alloc() != 0)
		goto fail;
	dx = o->dx;
	dy = o->dy;
	x0 = dx;
	y0 = dy;
	x1 = dx + (long long)sw * zx;
	y1 = dy + (long long)sh * zy;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > P.w)
		x1 = P.w;
	if (y1 > P.h)
		y1 = P.h;
	if (x1 <= x0 || y1 <= y0)
		goto fail;
	term_plane_prime_cells((int)x0, (int)y0, (int)x1, (int)y1);
	for (y = (int)y0; y < (int)y1; y++)
	{
		long long ly = (long long)y - dy;
		int syi = (int)(ly / zy);
		int yy;

		if (syi < 0 || syi >= sh)
			continue;
		if (o->fy)
			syi = sh - 1 - syi;
		yy = sy + syi;
		for (x = (int)x0; x < (int)x1; x++)
		{
			long long lx = (long long)x - dx;
			int sxi = (int)(lx / zx);
			int xx;
			const unsigned char *p;

			if (sxi < 0 || sxi >= sw)
				continue;
			if (o->fx)
				sxi = sw - 1 - sxi;
			if (have_mask)
			{
				int mi = (o->my + syi) * mask.w + o->mx + sxi;

				if (!(mask.bits[mi >> 3] &
				      (unsigned char)(0x80u >> (mi & 7u))))
					continue;
			}
			xx = sx + sxi;
			p = img->rgb + ((size_t)yy * (size_t)img->w +
					(size_t)xx) * 3u;
			P.pix[y * P.stride + x] = mmb_pix_store(
				((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) |
				(unsigned)p[2]);
			P.cov[(y / T.cell_h) * TM_MAX_COLS + x / TM_CW] = 1;
		}
	}
	if (have_mask)
	{
		/* Coverage is cell-granular.  The rect was primed from the
		 * current screen, so claiming the whole rect composites those
		 * glyph pixels under transparent mask bits instead of dropping
		 * to the glyph path (which would paint over the image). */
		int r0 = (int)y0 / T.cell_h;
		int r1 = ((int)y1 - 1) / T.cell_h;
		int c0 = (int)x0 / TM_CW;
		int c1 = ((int)x1 - 1) / TM_CW;
		int r, c;

		for (r = r0; r <= r1; r++)
			for (c = c0; c <= c1; c++)
				if (r >= 0 && r < T.pane_rows && c >= 0 &&
				    c < term_width())
					P.cov[r * TM_MAX_COLS + c] = 1;
	}
	T.img_x = T.pane_left * TM_CW + (int)x0;
	T.img_y = T.pane_top + (int)y0;
	T.img_w = (int)(x1 - x0);
	T.img_h = (int)(y1 - y0);
	T.img_lo = (int)(y0 / T.cell_h);
	T.img_hi = (int)((y1 - 1) / T.cell_h);
	if (T.img_hi >= T.pane_rows)
		T.img_hi = T.pane_rows - 1;
	for (y = T.img_lo; y <= T.img_hi; y++)
		mark_dirty_row(y);
	T.img_dirty = 1;
	T.need_draw = 1;
fail:
	if (have_mask && mask.owned)
		free(mask.bits);
}

static void term_apc_image(int jxl, const unsigned char *data, unsigned len,
			   const term_draw_opts *o)
{
	mmb_img_rgb img;
	int rc;

	memset(&img, 0, sizeof(img));
	term_yield();
	rc = jxl ? mmb_jxl_decode(data, len, 4000000u, &img)
		 : mmb_ppm_decode(data, len, 4000000u, &img);
	if (rc == 0)
		term_draw_image(&img, o);
	mmb_img_free(&img);
	term_yield();
}

static void term_apc_store(const char *rest, unsigned len)
{
	const char *sep;
	char name[TM_CACHE_NAME];
	unsigned char *buf = NULL;
	unsigned nlen, blen, outlen;

	if (!rest || len < 1)
		return;
	{
		unsigned k;

		sep = NULL;
		for (k = 0; k < len; k++)
		{
			if (rest[k] == ';')
			{
				sep = rest + k;
				break;
			}
		}
	}
	if (!sep)
		return;
	nlen = (unsigned)(sep - rest);
	if (nlen < 1 || nlen >= TM_CACHE_NAME)
		return;
	memcpy(name, rest, nlen);
	name[nlen] = 0;
	if (!term_safe_name(name))
		return;
	blen = len - nlen - 1;
	if (tm_b64_decode(sep + 1, blen, TM_CACHE_FILE_MAX, &buf, &outlen) != 0)
		return;
	term_cache_store(name, buf, outlen);
	free(buf);
}

static int tm_copy_key(const char *t, size_t n, term_copy_opts *o)
{
	if (n < 3 || t[1] != '=')
		return 0;
	if (t[0] == 'X')
		return tm_opt_int(t + 2, n - 2, &o->x, 0) == 0 ? 1 : -1;
	if (t[0] == 'Y')
		return tm_opt_int(t + 2, n - 2, &o->y, 0) == 0 ? 1 : -1;
	if (t[0] == 'W')
		return tm_opt_int(t + 2, n - 2, &o->w, 0) == 0 ? 1 : -1;
	if (t[0] == 'H')
		return tm_opt_int(t + 2, n - 2, &o->h, 0) == 0 ? 1 : -1;
	if (t[0] == 'B')
		return tm_opt_int(t + 2, n - 2, &o->buf, 0) == 0 ? 1 : -1;
	return 0;
}

/* P;Copy has only options, no trailing filename/payload. */
static int tm_copy_parse(const char *p, term_copy_opts *o)
{
	while (p && *p == ';')
	{
		const char *t = p + 1;
		const char *e = strchr(t, ';');
		size_t n = e ? (size_t)(e - t) : strlen(t);

		if (tm_copy_key(t, n, o) != 1)
			return -1;
		if (!e)
			return 0;
		p = e;
	}
	return p && *p == 0 ? 0 : -1;
}

static void term_apc_copy(const char *rest)
{
	term_copy_opts o;
	mmb_img_rgb nb;
	int x, y, w, h, yy, xx;

	memset(&o, 0, sizeof(o));
	if (tm_copy_parse(rest, &o) != 0)
		return;
	if (o.buf < 0 || o.buf >= TM_PIXBUF_COUNT)
		return;
	if (term_plane_alloc() != 0)
		return;
	x = o.x;
	y = o.y;
	if (x > P.w)
		x = P.w;
	if (y > P.h)
		y = P.h;
	w = o.w > 0 ? o.w : P.w - x;
	h = o.h > 0 ? o.h : P.h - y;
	if (w > P.w - x)
		w = P.w - x;
	if (h > P.h - y)
		h = P.h - y;
	if (w < 1 || h < 1)
		return;
	if ((unsigned long long)(unsigned)w * (unsigned)h >
	    TM_PIXBUF_MAX_PIXELS)
		return;
	memset(&nb, 0, sizeof(nb));
	nb.rgb = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
	if (!nb.rgb)
		return;
	nb.w = w;
	nb.h = h;
	/* Prime uncovered cells from the live screen so a copy includes text
	 * as well as image-plane pixels. */
	term_plane_prime_cells(x, y, x + w, y + h);
	for (yy = 0; yy < h; yy++)
	{
		const uint16_t *src = P.pix + (size_t)(y + yy) * P.stride + x;
		unsigned char *dst = nb.rgb + (size_t)yy * (size_t)w * 3u;

		for (xx = 0; xx < w; xx++)
		{
			unsigned c = mmb_native_to_rgb(src[xx]);

			dst[xx * 3] = (unsigned char)(c >> 16);
			dst[xx * 3 + 1] = (unsigned char)(c >> 8);
			dst[xx * 3 + 2] = (unsigned char)c;
		}
	}
	term_pixbuf_free_one(g_console, o.buf);
	PB[o.buf] = nb;
}

/* C;LoadPPM/LoadJXL/LoadPBM and their Blob forms: options are B= only (PBM
 * has no buffer selector), then a cache name or the base64 payload. */
static void term_apc_load(int jxl, int blob, int mask_only, const char *rest)
{
	term_draw_opts o;
	const char *payload;
	unsigned char *buf = NULL;
	unsigned outlen = 0;

	memset(&o, 0, sizeof(o));
	o.zx = 1;
	o.zy = 1;
	payload = tm_draw_payload(NULL, 0, rest, &o);
	if (!payload || !*payload)
		return;
	if (blob)
	{
		if (tm_b64_decode(payload, (unsigned)strlen(payload),
				  TM_APC_MAX, &buf, &outlen) != 0)
			return;
	}
	else
	{
		term_cache_ent *e;

		if (!term_safe_name(payload))
			return;
		e = term_cache_find(payload);
		if (!e)
			return;
		buf = e->data;
		outlen = e->len;
	}
	if (mask_only)
	{
		term_mask nm;

		memset(&nm, 0, sizeof(nm));
		if (term_mask_from_pbm(buf, outlen, &nm) == 0)
		{
			term_mask_free(g_console);
			TMASK = nm;
		}
	}
	else if (o.buf >= 0 && o.buf < TM_PIXBUF_COUNT)
	{
		mmb_img_rgb img;
		int rc;

		memset(&img, 0, sizeof(img));
		rc = jxl ? mmb_jxl_decode(buf, outlen, TM_PIXBUF_MAX_PIXELS,
					  &img)
			 : mmb_ppm_decode(buf, outlen, TM_PIXBUF_MAX_PIXELS,
					  &img);
		if (rc == 0)
		{
			term_pixbuf_free_one(g_console, o.buf);
			PB[o.buf] = img;
		}
	}
	if (blob)
		free(buf);
}

static void term_apc_dispatch(const char *body, unsigned len)
{
	const char *p;
	unsigned rem;
	term_draw_opts o;
	const char *payload;

	if (!body || len < 9 || memcmp(body, "SyncTERM:", 9) != 0)
		return;
	p = body + 9;
	rem = len - 9;
	if (rem >= 5 && memcmp(p, "Q;JXL", 5) == 0)
	{
		ansi_reply("\x1b[=1;1-n");
		return;
	}
	if (rem >= 4 && memcmp(p, "C;S;", 4) == 0)
	{
		term_apc_store(p + 4, rem - 4);
		return;
	}
	if (rem >= 3 && memcmp(p, "C;L", 3) == 0 &&
	    (rem == 3 || p[3] == ';'))
	{
		const char *pat = rem == 3 ? "*" : p + 4;

		term_apc_list(pat);
		return;
	}
	if (rem >= sizeof("C;LoadJXLBlob") - 1 &&
	    memcmp(p, "C;LoadJXLBlob", sizeof("C;LoadJXLBlob") - 1) == 0)
	{
		term_apc_load(1, 1, 0, p + sizeof("C;LoadJXLBlob") - 1);
		return;
	}
	if (rem >= sizeof("C;LoadJXL") - 1 &&
	    memcmp(p, "C;LoadJXL", sizeof("C;LoadJXL") - 1) == 0)
	{
		term_apc_load(1, 0, 0, p + sizeof("C;LoadJXL") - 1);
		return;
	}
	if (rem >= sizeof("C;LoadPPMBlob") - 1 &&
	    memcmp(p, "C;LoadPPMBlob", sizeof("C;LoadPPMBlob") - 1) == 0)
	{
		term_apc_load(0, 1, 0, p + sizeof("C;LoadPPMBlob") - 1);
		return;
	}
	if (rem >= sizeof("C;LoadPPM") - 1 &&
	    memcmp(p, "C;LoadPPM", sizeof("C;LoadPPM") - 1) == 0)
	{
		term_apc_load(0, 0, 0, p + sizeof("C;LoadPPM") - 1);
		return;
	}
	if (rem >= sizeof("C;LoadPBMBlob") - 1 &&
	    memcmp(p, "C;LoadPBMBlob", sizeof("C;LoadPBMBlob") - 1) == 0)
	{
		term_apc_load(0, 1, 1, p + sizeof("C;LoadPBMBlob") - 1);
		return;
	}
	if (rem >= sizeof("C;LoadPBM") - 1 &&
	    memcmp(p, "C;LoadPBM", sizeof("C;LoadPBM") - 1) == 0)
	{
		term_apc_load(0, 0, 1, p + sizeof("C;LoadPBM") - 1);
		return;
	}
	if (rem >= sizeof("P;Copy") - 1 &&
	    memcmp(p, "P;Copy", sizeof("P;Copy") - 1) == 0)
	{
		term_apc_copy(p + sizeof("P;Copy") - 1);
		return;
	}
	if (rem >= sizeof("P;Paste") - 1 &&
	    memcmp(p, "P;Paste", sizeof("P;Paste") - 1) == 0)
	{
		memset(&o, 0, sizeof(o));
		o.zx = 1;
		o.zy = 1;
		payload = tm_draw_payload(body, len,
					  p + sizeof("P;Paste") - 1, &o);
		if (payload)
			return;
		if (o.buf < 0 || o.buf >= TM_PIXBUF_COUNT || !PB[o.buf].rgb)
			return;
		term_draw_image(&PB[o.buf], &o);
		return;
	}
	memset(&o, 0, sizeof(o));
	o.zx = 1;
	o.zy = 1;
	if (rem >= sizeof("C;DrawJXLBlob") - 1 &&
	    memcmp(p, "C;DrawJXLBlob", sizeof("C;DrawJXLBlob") - 1) == 0)
	{
		p += sizeof("C;DrawJXLBlob") - 1;
		payload = tm_draw_payload(body, len, p, &o);
		if (!payload)
			return;
		{
			unsigned char *buf = NULL;
			unsigned outlen;

			if (tm_b64_decode(payload,
					  (unsigned)strlen(payload),
					  TM_APC_MAX, &buf, &outlen) != 0)
				return;
			term_apc_image(1, buf, outlen, &o);
			free(buf);
		}
		return;
	}
	if (rem >= sizeof("C;DrawJXL") - 1 &&
	    memcmp(p, "C;DrawJXL", sizeof("C;DrawJXL") - 1) == 0)
	{
		term_cache_ent *e;

		p += sizeof("C;DrawJXL") - 1;
		payload = tm_draw_payload(body, len, p, &o);
		if (!payload || !term_safe_name(payload))
			return;
		e = term_cache_find(payload);
		if (e)
			term_apc_image(1, e->data, e->len, &o);
		return;
	}
	if (rem >= sizeof("C;DrawPPMBlob") - 1 &&
	    memcmp(p, "C;DrawPPMBlob", sizeof("C;DrawPPMBlob") - 1) == 0)
	{
		p += sizeof("C;DrawPPMBlob") - 1;
		payload = tm_draw_payload(body, len, p, &o);
		if (!payload)
			return;
		{
			unsigned char *buf = NULL;
			unsigned outlen;

			if (tm_b64_decode(payload,
					  (unsigned)strlen(payload),
					  TM_APC_MAX, &buf, &outlen) != 0)
				return;
			term_apc_image(0, buf, outlen, &o);
			free(buf);
		}
		return;
	}
	if (rem >= sizeof("C;DrawPPM") - 1 &&
	    memcmp(p, "C;DrawPPM", sizeof("C;DrawPPM") - 1) == 0)
	{
		term_cache_ent *e;

		p += sizeof("C;DrawPPM") - 1;
		payload = tm_draw_payload(body, len, p, &o);
		if (!payload || !term_safe_name(payload))
			return;
		e = term_cache_find(payload);
		if (e)
			term_apc_image(0, e->data, e->len, &o);
		return;
	}
}

static void term_apc_buf_free(int idx)
{
	if (A_s[idx])
		free(A_s[idx]);
	A_s[idx] = 0;
	A_cap_s[idx] = 0;
}

static int term_apc_buf_put(unsigned char b)
{
	unsigned cap;

	if (T.apc_n + 1 >= TM_APC_MAX)
		return -1;
	if (T.apc_n + 2 > A_cap)
	{
		unsigned char *p;

		cap = A_cap ? A_cap : 4096;
		while (cap < T.apc_n + 2 && cap < TM_APC_MAX + 1)
			cap *= 2;
		if (cap > TM_APC_MAX + 1)
			cap = TM_APC_MAX + 1;
		p = (unsigned char *)realloc(A, cap);
		if (!p)
			return -1;
		A = p;
		A_cap = cap;
	}
	A[T.apc_n++] = b;
	A[T.apc_n] = 0;
	return 0;
}

/* ---- sixel image session --------------------------------------------- */

/* Make room before a six-pixel band runs past the bottom of the scrolling
 * region.  Each scroll moves the text cells and the plane up one text row,
 * and the image's coordinates follow, so the visible part is the bottom. */
static void term_sixel_ensure_room(void)
{
	int limit, region_rows;

	if (!T.six_ok || !T.sixel_scroll || T.scroll_bot <= T.scroll_top)
		return;
	limit = (T.scroll_bot + 1) * T.cell_h;
	region_rows = T.scroll_bot - T.scroll_top + 1;
	while (T.six.y + 6 > limit && T.six_scrolled < region_rows)
	{
		pane_scroll_up();
		T.six.y -= T.cell_h;
		T.six.bottom -= T.cell_h;
		if (T.six.bottom < 0)
			T.six.bottom = 0;
		T.six_top -= T.cell_h;
		if (T.six_top < 0)
			T.six_top = 0;
		T.six_scrolled++;
	}
}

static void term_sixel_begin(int p1, int p2, int p3)
{
	sixel_config cfg;
	int ox, oy;

	T.six_on = 0;
	T.six_ok = 0;
	if (term_plane_alloc() != 0)
		return;      /* discard the DCS payload; never print it */
	if (T.sixel_scroll)
	{
		ox = T.cur_col * TM_CW;
		oy = T.cur_row * T.cell_h;
	}
	else
	{
		/* DECSDM: pin the image at the pane origin. */
		ox = 0;
		oy = 0;
	}
	if (ox < 0)
		ox = 0;
	if (oy < 0)
		oy = 0;
	if (ox > P.w)
		ox = P.w;
	if (oy > P.h)
		oy = P.h;
	memset(&cfg, 0, sizeof cfg);
	cfg.pix = P.pix;
	cfg.stride = P.stride;
	cfg.width = P.w;
	cfg.height = P.h;
	cfg.ox = ox;
	cfg.oy = 0;
	cfg.p1 = p1;
	cfg.p2 = p2;
	cfg.p3 = p3;
	cfg.bg = mmb_pix_store(term_paper());
	cfg.pack = mmb_pix_store;
	cfg.cover = P.cov;
	cfg.cover_cols = TM_MAX_COLS;
	cfg.cell_w = TM_CW;
	cfg.cell_h = T.cell_h;
	sixel_begin(&T.six, &cfg);
	T.six.y = oy;
	T.six.bottom = oy;
	T.six.right = ox;
	T.six.maxh = T.sixel_scroll ? (T.scroll_bot + 1) * T.cell_h : P.h;
	if (T.six.maxh > P.h)
		T.six.maxh = P.h;
	T.six_top = oy;
	T.six_origin_row = T.cur_row;
	T.six_scrolled = 0;
	T.six_on = 1;
	T.six_ok = 1;
}

static void term_sixel_feed(unsigned char b)
{
	if (!T.six_on)
		return;
	term_sixel_ensure_room();
	sixel_feed(&T.six, b);
}

static void term_sixel_end(void)
{
	int row, left, right, top, bot, r;

	if (!T.six_on)
		return;
	T.six_on = 0;
	if (!T.six_ok)
		return;
	sixel_finish(&T.six);
	T.six_ok = 0;
	if (T.six.painted_w <= 0 || T.six.painted_h <= 0)
		return;

	if (T.sixel_scroll)
	{
		/* Cursor to column 0 of the cell row below the image. */
		row = (T.six.bottom + T.cell_h - 1) / T.cell_h;
		if (row < 0)
			row = 0;
		if (row >= T.pane_rows)
			row = T.pane_rows - 1;
		T.cur_row = row;
		T.cur_col = 0;
	}

	left = T.six.ox;
	right = T.six.right;
	top = T.six_top;
	bot = T.six.bottom;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > P.w)
		right = P.w;
	if (bot > P.h)
		bot = P.h;
	if (right > left && bot > top)
	{
		T.img_x = T.pane_left * TM_CW + left;
		T.img_y = T.pane_top + top;
		T.img_w = right - left;
		T.img_h = bot - top;
		T.img_lo = top / T.cell_h;
		T.img_hi = (bot - 1) / T.cell_h;
		if (T.img_hi >= T.pane_rows)
			T.img_hi = T.pane_rows - 1;
		T.img_dirty = 1;
		for (r = T.img_lo; r <= T.img_hi; r++)
			mark_dirty_row(r);
	}
	mark_dirty_row(T.cur_row);
	T.need_draw = 1;
}

static void ansi_reset_csi(void)
{
	int i;
	T.ansi_narg = 0;
	T.ansi_priv = 0;
	for (i = 0; i < TM_ANSI_ARGS; i++)
		T.ansi_arg[i] = 0;
}

static int ansi_dcs_open(void)
{
	return T.ansi_st == TM_ANSI_DCS_PARAM ||
	       T.ansi_st == TM_ANSI_DCS_BODY ||
	       T.ansi_st == TM_ANSI_DCS_ESC;
}

static void ansi_dcs_begin(void)
{
	ansi_reset_csi();
	T.ansi_st = TM_ANSI_DCS_PARAM;
	T.dcs_n = 0;
	T.dcs_sixel = 0;
	T.dcs_at = mmb_now_ms();
}

/* Leave a DCS without printing any of its tail. */
static void ansi_dcs_end(void)
{
	term_sixel_end();
	T.ansi_st = 0;
	T.dcs_sixel = 0;
}

/* Returns 1 when the byte belonged to an open DCS. */
static int ansi_feed(unsigned char b);

static int ansi_dcs_feed(unsigned char b)
{
	if (++T.dcs_n > TM_DCS_MAX)
	{
		ansi_dcs_end();
		return 1;
	}
	T.dcs_at = mmb_now_ms();
	if (T.ansi_st == TM_ANSI_DCS_PARAM)
	{
		if (b >= '0' && b <= '9')
		{
			if (T.ansi_narg == 0)
				T.ansi_narg = 1;
			T.ansi_arg[T.ansi_narg - 1] =
				T.ansi_arg[T.ansi_narg - 1] * 10 + (b - '0');
			return 1;
		}
		if (b == ';')
		{
			if (T.ansi_narg < TM_ANSI_ARGS)
				T.ansi_narg++;
			return 1;
		}
		if (b == '?')
		{
			T.ansi_priv = 1;
			return 1;
		}
		if (b >= '@' && b <= '~')
		{
			T.dcs_sixel = (b == 'q');
			if (T.dcs_sixel)
				term_sixel_begin(T.ansi_arg[0], T.ansi_arg[1],
						 T.ansi_arg[2]);
			T.ansi_st = TM_ANSI_DCS_BODY;
			return 1;
		}
		return 1;
	}
	if (T.ansi_st == TM_ANSI_DCS_ESC)
	{
		if (b == '\\')
		{
			ansi_dcs_end();
			return 1;
		}
		/* A lone ESC aborts the image, and the ESC itself is fed back so a
		 * missing terminator cannot eat the rest of the session. */
		ansi_dcs_end();
		ansi_feed(27);
		return ansi_feed(b) ? 1 : 0;
	}
	/* Body. */
	if (b == 0x9C || b == 0x18 || b == 0x1A)
	{
		ansi_dcs_end();
		return 1;
	}
	if (b == 27)
	{
		T.ansi_st = TM_ANSI_DCS_ESC;
		return 1;
	}
	if (T.dcs_sixel)
		term_sixel_feed(b);
	return 1;
}

static int ansi_apc_open(void)
{
	return T.ansi_st == TM_ANSI_APC_BODY ||
	       T.ansi_st == TM_ANSI_APC_ESC ||
	       T.ansi_st == TM_ANSI_STR_BODY ||
	       T.ansi_st == TM_ANSI_STR_ESC;
}

static void ansi_apc_begin(int store)
{
	ansi_reset_csi();
	T.ansi_st = store ? TM_ANSI_APC_BODY : TM_ANSI_STR_BODY;
	T.apc_n = 0;
	T.apc_store = store;
	T.apc_at = mmb_now_ms();
}

static void ansi_apc_end(void)
{
	int store = T.apc_store;
	unsigned n = T.apc_n;

	T.ansi_st = 0;
	T.apc_store = 0;
	T.apc_n = 0;
	if (store && n && A_s[g_console])
		term_apc_dispatch((const char *)A, n);
}

static int ansi_apc_feed(unsigned char b)
{
	if (T.ansi_st == TM_ANSI_APC_ESC || T.ansi_st == TM_ANSI_STR_ESC)
	{
		if (b == '\\')
		{
			ansi_apc_end();
			return 1;
		}
		ansi_apc_end();
		ansi_feed(27);
		return ansi_feed(b) ? 1 : 0;
	}
	if (T.apc_n >= TM_APC_MAX)
	{
		ansi_apc_end();
		return 1;
	}
	T.apc_at = mmb_now_ms();
	if (b == 0x9C || b == 0x18 || b == 0x1A)
	{
		ansi_apc_end();
		return 1;
	}
	if (b == 27)
	{
		T.ansi_st = (T.ansi_st == TM_ANSI_APC_BODY)
				    ? TM_ANSI_APC_ESC
				    : TM_ANSI_STR_ESC;
		return 1;
	}
	if (T.ansi_st == TM_ANSI_APC_BODY)
	{
		if (b < 0x08 || (b > 0x0D && b < 0x20) || b > 0x7E)
		{
			ansi_apc_end();
			return ansi_feed(b) ? 1 : 0;
		}
		if (T.apc_store)
		{
			if (term_apc_buf_put(b) != 0)
			{
				ansi_apc_end();
				return 1;
			}
		}
		else
			T.apc_n++;
	}
	else
		T.apc_n++;
	return 1;
}

static int ansi_feed(unsigned char b)
{
	if (T.ansi_st == 0)
	{
		if (b == 27)
		{
			T.ansi_st = 1;
			return 1;
		}
		/* 8-bit C1 DCS (0x90) and ST (0x9C): img2sixel defaults to the
		 * 7-bit form, but accepting the C1 forms is cheap. */
		if (b == 0x90)
		{
			ansi_dcs_begin();
			return 1;
		}
		if (b == 0x9F)
		{
			ansi_apc_begin(1);
			return 1;
		}
		if (b == 0x9E || b == 0x98)
		{
			ansi_apc_begin(0);
			return 1;
		}
		if (b == 0x9C)
			return 1;
		return 0;
	}
	if (ansi_dcs_open())
		return ansi_dcs_feed(b);
	if (ansi_apc_open())
		return ansi_apc_feed(b);
	if (T.ansi_st == 1)
	{
		if (b == '[')
		{
			T.ansi_st = 2;
			ansi_reset_csi();
			return 1;
		}
		if (b == ']')
		{
			T.ansi_st = 4;
			T.ansi_at = mmb_now_ms();
			return 1;
		}
		if (b == 'P')
		{
			ansi_dcs_begin();
			return 1;
		}
		if (b == '_')
		{
			ansi_apc_begin(1);
			return 1;
		}
		if (b == '^' || b == 'X')
		{
			ansi_apc_begin(0);
			return 1;
		}
		if (b == '7')
		{
			T.sav_row = T.cur_row;
			T.sav_col = T.cur_col;
			T.ansi_st = 0;
			return 1;
		}
		if (b == '8')
		{
			ansi_cup(T.sav_row + 1, T.sav_col + 1);
			T.ansi_st = 0;
			return 1;
		}
		if (b == 'c')
		{
			term_reset_pen();
			term_set_cursor_vis(1);
			ansi_erase_disp(2);
			T.ansi_st = 0;
			return 1;
		}
		if (b == '(' || b == ')' || b == '*' || b == '+' || b == '-')
		{
			T.ansi_st = 7;
			return 1;
		}
		T.ansi_st = 0;
		return 1;
	}
	if (T.ansi_st == 7)
	{
		T.ansi_st = 0;
		return 1;
	}
	if (T.ansi_st == 4)
	{
		if (T.ansi_at && mmb_now_ms() - T.ansi_at > TM_ANSI_OSC_MS)
		{
			T.ansi_st = 0;
			return ansi_feed(b);
		}
		if (b == 7 || b == 27)
			T.ansi_st = (b == 27) ? 5 : 0;
		return 1;
	}
	if (T.ansi_st == 5)
	{
		T.ansi_st = 0;
		return 1;
	}
	if (b == '?')
	{
		T.ansi_priv = 1;
		return 1;
	}
	if (b == '<')
	{
		T.ansi_priv = 2;
		return 1;
	}
	if (b >= '0' && b <= '9')
	{
		if (T.ansi_narg == 0)
			T.ansi_narg = 1;
		T.ansi_arg[T.ansi_narg - 1] =
			T.ansi_arg[T.ansi_narg - 1] * 10 + (b - '0');
		return 1;
	}
	if (b == ';')
	{
		if (T.ansi_narg < TM_ANSI_ARGS)
			T.ansi_narg++;
		return 1;
	}
	/* ECMA-48 final byte is 0x40..0x7E; anything else ends the sequence with
	 * no effect.  `@` (ICH) is the common non-letter final byte. (#1223) */
	if (b >= '@' && b <= '~')
	{
		if (T.ansi_priv)
			ansi_exec_priv((char)b);
		else
			ansi_exec_csi((char)b);
		T.ansi_st = 0;
		return 1;
	}
	T.ansi_st = 0;
	return 1;
}

static int telopt_known(unsigned char o)
{
	return o == TELOPT_ECHO || o == TELOPT_SGA ||
	       o == TELOPT_TTYPE || o == TELOPT_NAWS;
}

static int iac_cmd_byte(unsigned char b)
{
	return b == IAC || b == WILL || b == WONT || b == DO || b == DONT ||
	       (b >= NOP && b <= GA);
}

static void ansi_watchdog(void)
{
	if (ansi_dcs_open())
	{
		if (T.dcs_at && mmb_now_ms() - T.dcs_at > TM_DCS_IDLE_MS)
			ansi_dcs_end();
		return;
	}
	if (ansi_apc_open())
	{
		if (T.apc_at && mmb_now_ms() - T.apc_at > TM_APC_IDLE_MS)
			ansi_apc_end();
		return;
	}
	if (T.ansi_st != 4 && T.ansi_st != 5)
		return;
	if (T.ansi_at && mmb_now_ms() - T.ansi_at > TM_ANSI_OSC_MS)
		T.ansi_st = 0;
}

static void sb_watchdog(void)
{
	if (!T.sb)
		return;
	if (T.sb_n > TM_SB_MAX)
	{
		T.sb = 0;
		T.iac = 0;
		return;
	}
	if (T.sb_at && mmb_now_ms() - T.sb_at > TM_SB_MS)
	{
		T.sb = 0;
		T.iac = 0;
	}
}

static void incoming_byte(unsigned char b)
{
	sb_watchdog();
	ansi_watchdog();
	if (T.sb)
	{
		T.sb_n++;
		if (T.iac == 1)
		{
			T.iac = 0;
			if (b == SE)
			{
				if (T.sb_opt == TELOPT_TTYPE && T.sb_cmd == TTYPE_SEND)
					send_ttype();
				T.sb = 0;
			}
		}
		else if (b == IAC)
			T.iac = 1;
		else if (T.sb_opt < 0)
			T.sb_opt = (int)b;
		else if (T.sb_cmd < 0)
			T.sb_cmd = (int)b;
		return;
	}
	if (T.iac == 1)
	{
		if (b == IAC)
		{
			T.iac = 0;
			pane_put((char)b);
			return;
		}
		if (b == SB)
		{
			T.iac = 0;
			T.sb = 1;
			T.sb_n = 0;
			T.sb_at = mmb_now_ms();
			T.sb_opt = -1;
			T.sb_cmd = -1;
			return;
		}
		if (b == WILL || b == WONT || b == DO || b == DONT)
		{
			T.iac_cmd = b;
			T.iac = 2;
			return;
		}
		T.iac = 0;
		return;
	}
	if (T.iac == 2)
	{
		apply_option(T.iac_cmd, b);
		T.iac = 0;
		return;
	}
	if (b == IAC)
	{
		T.iac = 1;
		return;
	}
	if (ansi_feed(b))
		return;
	if (b == 0)
		return;
	if (b == 12)
	{
		ansi_erase_disp(2);
		return;
	}
	if (b == '\r')
	{
		T.cur_col = 0;
		mark_dirty_row(T.cur_row);
	}
	else if (b == '\n')
		pane_newline();
	else if (b == 8 || b == 127)
		pane_rubout();
	else if (b >= 32)
		pane_put((char)b);
}

static void term_log_path(char *dst, unsigned n)
{
	if (mmb_fat_ready('C'))
		strncpy(dst, "C:/.termlog", n - 1);
	else
		strncpy(dst, "A:/.termlog", n - 1);
	dst[n - 1] = 0;
}

static void term_log_flush(void)
{
	char path[24];

	if (L.buf_n <= 0)
		return;
	term_log_path(path, sizeof(path));
	mmb_vfs_write(path, L.buf, (unsigned)L.buf_n, 1);
	L.buf_n = 0;
	L.flush_at = mmb_now_ms();
}

static void term_log_poll(void)
{
	if (!G.opt.term_log || L.buf_n <= 0)
		return;
	if (mmb_now_ms() - L.flush_at >= TM_LOG_FLUSH_MS)
		term_log_flush();
}

static void term_log_putc(char c)
{
	if (L.buf_n >= TM_LOG_BUF)
		term_log_flush();
	if (L.buf_n < TM_LOG_BUF)
		L.buf[L.buf_n++] = c;
}

static void term_log_u(unsigned v)
{
	char tmp[12];
	int i = 0;

	if (v == 0)
	{
		term_log_putc('0');
		return;
	}
	while (v && i < 11)
	{
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i > 0)
		term_log_putc(tmp[--i]);
}

static void term_log_line(char kind, const unsigned char *p, int n)
{
	static const char hexdig[] = "0123456789ABCDEF";
	int i, need;

	if (!G.opt.term_log || n <= 0)
		return;
	need = 36 + n * 2;
	if (need > TM_LOG_BUF)
		n = (TM_LOG_BUF - 36) / 2;
	if (L.buf_n + need > TM_LOG_BUF)
		term_log_flush();
	term_log_putc(kind);
	term_log_putc(' ');
	term_log_u(mmb_now_ms() - L.start_at);
	term_log_putc(' ');
	term_log_u(L.in_n);
	term_log_putc(' ');
	term_log_u(L.rendered);
	term_log_putc(' ');
	for (i = 0; i < n; i++)
	{
		term_log_putc(hexdig[p[i] >> 4]);
		term_log_putc(hexdig[p[i] & 15]);
	}
	term_log_putc('\n');
	if (L.buf_n >= TM_LOG_WATER)
		term_log_flush();
}

static void term_log_mark(void)
{
	if (G.opt.term_log)
		L.rendered = L.in_n;
}

static void term_log_rx(const unsigned char *p, int n)
{
	if (!G.opt.term_log || host_is_demo() || !p || n <= 0)
		return;
	L.in_n += (unsigned)n;
	term_log_line('R', p, n);
}

static void term_log_tx(const unsigned char *p, int n)
{
	if (!G.opt.term_log || host_is_demo() || !p || n <= 0)
		return;
	L.out_n += (unsigned)n;
	term_log_line('T', p, n);
}

static void term_log_event(const char *s)
{
	if (!G.opt.term_log || !s || !s[0])
		return;
	term_log_line('E', (const unsigned char *)s, (int)strlen(s));
}

void mmb_term_log_enable(int on)
{
	char path[24];

	if (term_log_alloc() != 0)
		return;
	if (!on)
	{
		term_log_flush();
		mmb_out("TERM log off in=");
		mmb_outf(0, (int64_t)L.in_n);
		mmb_out(" out=");
		mmb_outf(0, (int64_t)L.out_n);
		mmb_out(" rendered=");
		mmb_outf(0, (int64_t)L.rendered);
		return;
	}
	L.in_n = 0;
	L.out_n = 0;
	L.rendered = 0;
	L.buf_n = 0;
	L.start_at = mmb_now_ms();
	L.flush_at = L.start_at;
	term_log_path(path, sizeof(path));
	mmb_vfs_write(path, "# TERMLOG 2\n", 12, 0);
	mmb_out("TERM log ");
	mmb_out(path);
}

static void term_rx_consume(unsigned n)
{
	unsigned char tmp[256];
	unsigned got;

	while (n > 0)
	{
		got = n;
		if (got > sizeof tmp)
			got = sizeof tmp;
		got = mmb_net_rxbuf_pop(&term_rx, tmp, got);
		if (!got)
			return;
		T.parsed_n += got;
		term_log_rx(tmp, (int)got);
		n -= got;
	}
}

/*
 * ZMODEM downloads: auto-detected receive-only transfers.
 *
 * The engine (mmbasic/src/zmodem.c) is transport agnostic; this glue stores
 * files in the configured download folder, auto-renaming collisions, and
 * reports progress on the status row and over serial.
 */

static void zm_join(const char *dir, const char *base, char *out, unsigned outsz)
{
	unsigned i = 0, j;
	unsigned n = (unsigned)strlen(dir);

	for (j = 0; j < n && i + 1 < outsz; j++)
		out[i++] = dir[j];
	if (n > 0 && dir[n - 1] != '/' && i + 1 < outsz)
		out[i++] = '/';
	for (j = 0; base[j] && i + 1 < outsz; j++)
		out[i++] = base[j];
	out[i] = 0;
}

static void zm_base(const char *name, char *out, unsigned outsz)
{
	const char *base = name ? name : "";
	unsigned i, n = 0;

	for (i = 0; base[i]; i++)
		if (base[i] == '/' || base[i] == '\\')
			base = base + i + 1;
	for (i = 0; base[i] && n + 1 < outsz; i++)
	{
		unsigned char c = (unsigned char)base[i];
		if (c < 32 || c == 0x7F)
			continue;
		if (strchr("*?\"<>|:", (int)c))
			continue;
		out[n++] = (char)c;
	}
	out[n] = 0;
	if (!n)
	{
		strncpy(out, "DOWNLOAD.BIN", outsz - 1);
		out[outsz - 1] = 0;
	}
}

static void zm_pick_name(const char *base, char *out, unsigned outsz)
{
	const char *dir = g_dl_dir[0] ? g_dl_dir : mmb_vfs_cwd();
	char cand[160];
	const char *dot = 0;
	int i, j;

	zm_join(dir, base, cand, sizeof(cand));
	if (!mmb_vfs_exists(cand))
	{
		strncpy(out, cand, outsz - 1);
		out[outsz - 1] = 0;
		return;
	}
	for (i = 0; base[i]; i++)
		if (base[i] == '.')
			dot = base + i;
	for (j = 1; j < 100; j++)
	{
		char nb[160];

		nb[0] = 0;
		if (dot && dot != base)
		{
			unsigned k, n = (unsigned)(dot - base);
			if (n > sizeof(nb) - 20)
				n = sizeof(nb) - 20;
			for (k = 0; k < n; k++)
				nb[k] = base[k];
			nb[n] = 0;
			bm_append(nb, sizeof(nb), "-");
			bm_append_int(nb, sizeof(nb), j);
			bm_append(nb, sizeof(nb), dot);
		}
		else
		{
			bm_append(nb, sizeof(nb), base);
			bm_append(nb, sizeof(nb), "-");
			bm_append_int(nb, sizeof(nb), j);
		}
		zm_join(dir, nb, cand, sizeof(cand));
		if (!mmb_vfs_exists(cand))
			break;
	}
	strncpy(out, cand, outsz - 1);
	out[outsz - 1] = 0;
}

static int zm_open_cb(void *ctx, const char *name, unsigned size)
{
	char base[80], path[160];

	(void)ctx;
	(void)size;
	zm_base(name, base, sizeof(base));
	zm_pick_name(base, path, sizeof(path));
	if (mmb_vfs_write(path, "", 0, 0) != 0)
		return -1;
	strncpy(zm_cur_path, path, sizeof(zm_cur_path) - 1);
	zm_cur_path[sizeof(zm_cur_path) - 1] = 0;
	return 0;
}

static int zm_write_cb(void *ctx, const unsigned char *data, unsigned n)
{
	(void)ctx;
	if (!zm_cur_path[0])
		return -1;
	return mmb_vfs_write(zm_cur_path, data, n, 1) == 0 ? 0 : -1;
}

static void zm_close_cb(void *ctx, int ok)
{
	(void)ctx;
	if (!ok && zm_cur_path[0])
		mmb_vfs_kill(zm_cur_path);
	zm_cur_path[0] = 0;
}

static int zm_send_cb(void *ctx, const unsigned char *data, unsigned n)
{
	(void)ctx;
	term_log_tx(data, (int)n);
	term_net_send_raw(data, n);
	return 0;
}

static mmb_zm_ops zm_ops;

static void zm_setup(void)
{
	if (zm_ready)
		return;
	zm_ops.ctx = 0;
	zm_ops.open = zm_open_cb;
	zm_ops.write = zm_write_cb;
	zm_ops.close = zm_close_cb;
	zm_ops.send = zm_send_cb;
	mmb_zm_init(&ZM, &zm_ops);
	zm_ready = 1;
}

/* Rolling `ZPAD ZPAD ZDLE` detector: false positives are cheap because a
 * session without a valid ZRQINIT fails on the first bad header. */
static int zmodem_scan(unsigned char b)
{
	zm_shift = (zm_shift << 8) | b;
	return (zm_shift & 0xFFFFFFu) == 0x2A2A18u;
}

static void zmodem_draw_status(void)
{
	char line[80];
	char pct[16];
	const char *nm = mmb_zm_name(&ZM);
	unsigned total = 0, got = 0;
	int y = term_status_y(), n, x;
	unsigned fg = TM_MENU_FG, bg = TM_MENU_BG;

	line[0] = 0;
	if (ZM.state == MMB_ZM_ACTIVE)
	{
		got = mmb_zm_progress(&ZM, &total);
		bm_append(line, sizeof(line), "ZMODEM ");
		bm_append(line, sizeof(line), (nm && nm[0]) ? nm : "waiting");
		if (total > 0)
		{
			pct[0] = 0;
			bm_append(pct, sizeof(pct), " ");
			bm_append_int(pct, sizeof(pct),
				      (int)((unsigned long)got * 100u / total));
			bm_append(pct, sizeof(pct), "%");
			bm_append(line, sizeof(line), pct);
		}
	}
	else
	{
		strncpy(line, mmb_zm_status(&ZM), sizeof(line) - 1);
		line[sizeof(line) - 1] = 0;
	}
	if (strcmp(line, zm_shown) == 0)
		return;
	strncpy(zm_shown, line, sizeof(zm_shown) - 1);
	zm_shown[sizeof(zm_shown) - 1] = 0;
	mmb_gfx_fill_rect_topdown(0, y, T.vid_cols * TM_CW, T.cell_h, bg);
	n = (int)strlen(line);
	if (n > T.vid_cols)
		n = T.vid_cols;
	x = (T.vid_cols - n) * TM_CW;
	term_put_str_bg(x, y, line, fg, bg);
	term_present_async_rect(0, y, T.vid_cols * TM_CW, T.cell_h);
}

static void zmodem_start(void)
{
	zm_setup();
	mmb_zm_begin(&ZM);
	g_dl_last_name[0] = 0;
	g_dl_last_files = 0;
	zm_cur_path[0] = 0;
	zm_shown[0] = 0;
	/* The detector already consumed ZPAD ZPAD ZDLE; hand them back so the
	 * engine sees a complete frame prefix. */
	{
		static const unsigned char pre[3] = { 0x2A, 0x2A, 0x18 };
		mmb_zm_feed(&ZM, pre, 3, mmb_now_ms());
	}
}

static void zmodem_pump(void)
{
	unsigned char buf[256];
	unsigned n, now;

	if (!mmb_zm_active(&ZM))
		return;
	now = mmb_now_ms();
	while (mmb_net_rxbuf_used(&term_rx) > 0)
	{
		n = mmb_net_rxbuf_pop(&term_rx, buf, sizeof(buf));
		if (!n)
			break;
		T.parsed_n += n;
		term_log_rx(buf, (int)n);
		mmb_zm_feed(&ZM, buf, n, now);
		if (!mmb_zm_active(&ZM))
			break;
		term_yield();
	}
	mmb_zm_tick(&ZM, now);
}

static void zmodem_finished(void)
{
	int st = ZM.state;
	const char *nm = mmb_zm_name(&ZM);
	char line[128];

	line[0] = 0;
	if (st == MMB_ZM_DONE)
	{
		bm_append(line, sizeof(line), "ZMODEM complete: ");
		bm_append(line, sizeof(line), nm);
		ser("!ZMODEM done\r\n");
	}
	else if (st == MMB_ZM_CANCELLED)
	{
		strncpy(line, "ZMODEM cancelled", sizeof(line) - 1);
		ser("!ZMODEM cancelled\r\n");
	}
	else
	{
		strncpy(line, "ZMODEM failed", sizeof(line) - 1);
		ser("!ZMODEM failed\r\n");
	}
	term_log_event(line);
	pane_puts(line);
	pane_newline();
	mmb_zm_forget(&ZM);
	zm_shift = 0;
	mark_dirty_full();
	term_draw();
	term_serial_dump();
}

static void zmodem_poll(void)
{
	int got;

	if (T.tcp && !T.replay && !T.file_replay)
	{
		got = term_tcp_drain(TM_RECV_IDLE);
		if (got < 0)
			return;
	}
	zmodem_pump();
	if (!mmb_zm_active(&ZM))
	{
		zmodem_finished();
		return;
	}
	zmodem_draw_status();
	term_yield();
}

/* ---- download folder browser dialog ---------------------------------- */

static int term_dl_root(const char *dir)
{
	int n = (int)strlen(dir);
	return n >= 2 && n <= 3 && dir[1] == ':';
}

static void term_dl_scan(void)
{
	char list[2048];
	char *p;

	g_dl_n = 0;
	if (!g_dl_cur[0])
	{
		strncpy(g_dl_cur, mmb_vfs_cwd(), sizeof(g_dl_cur) - 1);
		g_dl_cur[sizeof(g_dl_cur) - 1] = 0;
	}
	if (!term_dl_root(g_dl_cur))
	{
		strncpy(g_dl_names[g_dl_n], "..", sizeof(g_dl_names[0]) - 1);
		g_dl_n++;
	}
	list[0] = 0;
	if (mmb_vfs_list(g_dl_cur, list, sizeof(list), &g_dl_trunc) == 0)
	{
		p = list;
		while (*p && g_dl_n < TM_DL_DIRS)
		{
			char name[64];
			int m = 0;
			while (*p && *p != '\n' && *p != '\r' &&
			       m < (int)sizeof(name) - 1)
				name[m++] = *p++;
			name[m] = 0;
			while (*p == '\r' || *p == '\n')
				p++;
			if (m > 0 && name[m - 1] == '/' && name[0] &&
			    name[0] != '.')
			{
				name[m - 1] = 0;
				strncpy(g_dl_names[g_dl_n], name,
					sizeof(g_dl_names[0]) - 1);
				g_dl_names[g_dl_n][sizeof(g_dl_names[0]) - 1] = 0;
				g_dl_n++;
			}
		}
	}
	if (g_dl_sel >= g_dl_n && g_dl_n > 0)
		g_dl_sel = g_dl_n - 1;
	if (g_dl_sel < 0)
		g_dl_sel = 0;
	g_dl_top = 0;
}

static void term_dl_use(void)
{
	char full[128];

	full[0] = 0;
	if (mmb_vfs_resolve(g_dl_cur, full, sizeof(full)) != 0)
	{
		strncpy(full, g_dl_cur, sizeof(full) - 1);
		full[sizeof(full) - 1] = 0;
	}
	if (!mmb_vfs_isdir(full))
		return;
	strncpy(g_dl_dir, full, sizeof(g_dl_dir) - 1);
	g_dl_dir[sizeof(g_dl_dir) - 1] = 0;
	term_bm_save();
	T.dlg = TM_DLG_NONE;
	term_ui_refresh();
}

static void term_dl_up(void)
{
	int n = (int)strlen(g_dl_cur);
	int i, slash = -1;

	for (i = 0; i < n; i++)
		if (g_dl_cur[i] == '/')
			slash = i;
	if (slash > 2)
		g_dl_cur[slash] = 0;
	if (g_dl_cur[0] && g_dl_cur[1] == ':' && g_dl_cur[2] == 0)
	{
		g_dl_cur[2] = '/';
		g_dl_cur[3] = 0;
	}
	g_dl_sel = 0;
	term_dl_scan();
	term_dlg_refresh();
}

static void term_dl_enter(void)
{
	if (g_dl_focus == TM_DL_USE)
	{
		term_dl_use();
		return;
	}
	if (g_dl_focus == TM_DL_UP)
	{
		term_dl_up();
		return;
	}
	if (g_dl_focus == TM_DL_CANCEL)
	{
		T.dlg = TM_DLG_NONE;
		term_ui_refresh();
		return;
	}
	if (g_dl_n <= 0)
		return;
	if (strcmp(g_dl_names[g_dl_sel], "..") == 0)
	{
		term_dl_up();
		return;
	}
	{
		char next[128], full[128];

		zm_join(g_dl_cur, g_dl_names[g_dl_sel], next, sizeof(next));
		full[0] = 0;
		if (mmb_vfs_resolve(next, full, sizeof(full)) != 0 ||
		    !mmb_vfs_isdir(full))
		{
			strncpy(full, next, sizeof(full) - 1);
			full[sizeof(full) - 1] = 0;
		}
		strncpy(g_dl_cur, full, sizeof(g_dl_cur) - 1);
		g_dl_cur[sizeof(g_dl_cur) - 1] = 0;
	}
	g_dl_sel = 0;
	term_dl_scan();
	term_dlg_refresh();
}

static void term_dl_draw(void)
{
	int i, ch = 18;

	term_dlg_frame(60, ch, g_dl_trunc ? " Download folder (more) "
					  : " Download folder ");
	dlg_text_at(2, 2, g_dl_cur, 0);
	if (g_dl_sel < g_dl_top)
		g_dl_top = g_dl_sel;
	if (g_dl_sel >= g_dl_top + TM_DL_VIEW)
		g_dl_top = g_dl_sel - TM_DL_VIEW + 1;
	if (g_dl_top < 0)
		g_dl_top = 0;
	for (i = 0; i < TM_DL_VIEW; i++)
	{
		int idx = g_dl_top + i;
		if (idx >= g_dl_n)
			break;
		dlg_field(2, 4 + i, 56, g_dl_names[idx],
			  g_dl_focus == TM_DL_LIST && idx == g_dl_sel);
	}
	dlg_btn(2, ch - 2, "Use", g_dl_focus == TM_DL_USE);
	dlg_btn(10, ch - 2, "Up", g_dl_focus == TM_DL_UP);
	dlg_btn(18, ch - 2, "Cancel", g_dl_focus == TM_DL_CANCEL);
}

static void term_dl_serial(void)
{
	int i;

	ser("Download folder\r\n");
	ser(g_dl_cur);
	ser("\r\n");
	for (i = 0; i < g_dl_n && i < 8; i++)
	{
		ser(g_dl_names[i]);
		ser("\r\n");
	}
	ser("Use\r\nUp\r\nCancel\r\n");
}

static int term_dl_key(char c)
{
	if (c == '\r' || c == '\n')
	{
		term_dl_enter();
		return 1;
	}
	if (c == '\t')
	{
		g_dl_focus = (g_dl_focus + 1) % 4;
		term_dlg_refresh();
		return 1;
	}
	return 1;
}

static void term_dl_arrow(int c)
{
	if (g_dl_focus == TM_DL_LIST)
	{
		if (c == 'A' && g_dl_n > 0)
		{
			g_dl_sel--;
			if (g_dl_sel < 0)
				g_dl_sel = g_dl_n - 1;
		}
		else if (c == 'B' && g_dl_n > 0)
		{
			g_dl_sel++;
			if (g_dl_sel >= g_dl_n)
				g_dl_sel = 0;
		}
		else if (c == 'C')
			g_dl_focus = TM_DL_USE;
		else if (c == 'D')
			g_dl_focus = TM_DL_CANCEL;
	}
	else
	{
		if (c == 'A' || c == 'D')
			g_dl_focus = (g_dl_focus + 3) % 4;
		else if (c == 'B' || c == 'C')
			g_dl_focus = (g_dl_focus + 1) % 4;
	}
	term_dlg_refresh();
}

static void term_dl_open(void)
{
	term_bm_load();
	if (!g_dl_dir[0])
	{
		strncpy(g_dl_dir, mmb_vfs_cwd(), sizeof(g_dl_dir) - 1);
		g_dl_dir[sizeof(g_dl_dir) - 1] = 0;
	}
	g_dl_cur[0] = 0;
	if (mmb_vfs_resolve(g_dl_dir, g_dl_cur, sizeof(g_dl_cur)) != 0)
	{
		strncpy(g_dl_cur, mmb_vfs_cwd(), sizeof(g_dl_cur) - 1);
		g_dl_cur[sizeof(g_dl_cur) - 1] = 0;
	}
	T.menu = 0;
	T.dlg = TM_DLG_DL;
	g_dl_sel = 0;
	g_dl_focus = TM_DL_LIST;
	term_dl_scan();
	term_ui_refresh();
}

static int term_tcp_drain(int idle_max)
{
	static unsigned char buf[TM_RECV_BUF];
	static int draining;
	int n, loops, idle, got;
	unsigned space, want;

	if (!T.tcp || T.replay || T.file_replay)
		return 0;
	if (idle_max < 1)
		idle_max = 1;
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	if (draining)
		return 0;
	draining = 1;
	idle = 0;
	got = 0;
	for (loops = 0; loops < TM_RECV_LOOPS; loops++)
	{
		space = mmb_net_rxbuf_free(&term_rx);
		if (!space)
			break;
		want = sizeof(buf);
		if (want > space)
			want = space;
		n = mmb_net_tcp_recv(buf, want);
		if (n < 0)
		{
			draining = 0;
			tcp_lost(mmb_net_tcp_close_reason());
			return -1;
		}
		if (n == 0)
		{
			term_yield();
			if (++idle >= idle_max)
				break;
			continue;
		}
		idle = 0;
		mmb_net_rxbuf_push(&term_rx, buf, (unsigned)n);
		T.rx_n += (unsigned)n;
		got += n;
		term_yield();
	}
	draining = 0;
	return got;
}

static int term_tcp_ingest(int idle_max)
{
	int got;

	got = term_tcp_drain(idle_max);
	if (got < 0)
		return -1;
	if (mmb_zm_active(&ZM))
	{
		zmodem_pump();
		return got;
	}
	if (got || mmb_net_rxbuf_used(&term_rx))
		term_rx_interpret();
	if (mmb_zm_active(&ZM))
		zmodem_pump();
	iac_flush();
	return got;
}

static void term_maybe_serial_dump(int got)
{
	if (T.live)
	{
		if (!mmb_opt_wifi_debug() || got <= 0)
			return;
		if (!T.dump_at || mmb_now_ms() - T.dump_at >= TM_DUMP_MS)
		{
			term_net_stats(0);
			T.dump_at = mmb_now_ms();
		}
		return;
	}
	if (got <= 0)
		return;
	if (got < 80 || !T.dump_at ||
	    mmb_now_ms() - T.dump_at >= TM_DUMP_MS)
	{
		term_serial_dump();
		T.dump_at = mmb_now_ms();
	}
}

static int term_rx_interpret(void)
{
	int got = 0;

	if (mmb_zm_active(&ZM))
	{
		zmodem_pump();
		return 0;
	}
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	while (mmb_net_rxbuf_used(&term_rx) > 0)
	{
		unsigned char b = mmb_net_rxbuf_at(&term_rx, 0);
		unsigned avail = mmb_net_rxbuf_used(&term_rx);

		if (!ansi_dcs_open() && zmodem_scan(b))
		{
			term_rx_consume(1);
			zmodem_start();
			zmodem_pump();
			return got;
		}
		sb_watchdog();
		ansi_watchdog();
		if (T.sb || T.iac)
		{
			incoming_byte(b);
			term_rx_consume(1);
			got++;
			if ((got & (TM_INTERP_YIELD - 1)) == 0)
			{
				term_yield();
				if (T.tcp && !T.replay && !T.file_replay &&
				    term_tcp_drain(1) < 0)
					return got;
			}
			continue;
		}
		if (b != IAC)
		{
			incoming_byte(b);
			term_rx_consume(1);
			got++;
			if ((got & (TM_INTERP_YIELD - 1)) == 0)
			{
				term_yield();
				if (T.tcp && !T.replay && !T.file_replay &&
				    term_tcp_drain(1) < 0)
					return got;
			}
			continue;
		}
		if (avail < 2)
			break;
		{
			unsigned char b1 = mmb_net_rxbuf_at(&term_rx, 1);

			if (b1 == SB)
			{
				if (avail < 3)
					break;
				if (telopt_known(mmb_net_rxbuf_at(&term_rx, 2)))
				{
					incoming_byte(IAC);
					term_rx_consume(1);
					got++;
					continue;
				}
				pane_put((char)IAC);
				term_rx_consume(1);
				got++;
				continue;
			}
			if (iac_cmd_byte(b1))
			{
				incoming_byte(IAC);
				term_rx_consume(1);
				got++;
				continue;
			}
			pane_put((char)IAC);
			term_rx_consume(1);
			got++;
		}
	}
	return got;
}

static void incoming_feed(const unsigned char *src, int n)
{
	unsigned now;

	if (!src || n <= 0)
		return;
	if (mmb_zm_active(&ZM))
	{
		now = mmb_now_ms();
		T.parsed_n += (unsigned)n;
		term_log_rx(src, n);
		mmb_zm_feed(&ZM, src, (unsigned)n, now);
		mmb_zm_tick(&ZM, now);
		return;
	}
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	while (n > 0)
	{
		unsigned w = mmb_net_rxbuf_push(&term_rx, src, (unsigned)n);

		if (!w)
		{
			if (!term_rx_interpret())
				return;
			iac_flush();
			continue;
		}
		src += w;
		n -= (int)w;
	}
	term_rx_interpret();
	/* Replay/file input bypasses term_tcp_ingest; flush any telnet
	 * negotiation responses queued while interpreting it. */
	iac_flush();
}

static int file_hex(const char *s, unsigned char *dst, int maxn)
{
	int n = 0, hi, lo;

	while (s[0] && s[1] && n < maxn)
	{
		if (s[0] == ' ' || s[0] == '\t')
		{
			s++;
			continue;
		}
		hi = hexval(s[0]);
		lo = hexval(s[1]);
		if (hi < 0 || lo < 0)
			break;
		dst[n++] = (unsigned char)((hi << 4) | lo);
		s += 2;
	}
	return n;
}

static int file_fill(void)
{
	unsigned got, room;

	if (T.file_line_n < 0)
		T.file_line_n = 0;
	room = (unsigned)(TM_FILE_LINE - 1 - T.file_line_n);
	if (!room)
		return 1;
	if ((int)T.file_pos >= T.file_sz)
		return T.file_line_n > 0 ? 1 : 0;
	if (mmb_vfs_read_at(T.file_path, T.file_pos, T.file_line + T.file_line_n,
			    room, &got) != 0)
		return -1;
	if (!got)
		return T.file_line_n > 0 ? 1 : 0;
	T.file_pos += got;
	T.file_line_n += (int)got;
	T.file_line[T.file_line_n] = 0;
	return 1;
}

static int file_next_line(char *dst, int dstsz)
{
	char *nl;
	int n, skip;

	for (;;)
	{
		T.file_line[T.file_line_n] = 0;
		nl = strchr(T.file_line, '\n');
		if (nl)
			break;
		skip = file_fill();
		if (skip < 0)
			return -1;
		if (skip == 0)
		{
			if (T.file_line_n <= 0)
				return 0;
			n = T.file_line_n;
			if (n >= dstsz)
				n = dstsz - 1;
			memcpy(dst, T.file_line, (unsigned)n);
			dst[n] = 0;
			T.file_line_n = 0;
			return 1;
		}
		if (!strchr(T.file_line, '\n') && T.file_line_n >= TM_FILE_LINE - 1)
		{
			n = T.file_line_n;
			if (n >= dstsz)
				n = dstsz - 1;
			memcpy(dst, T.file_line, (unsigned)n);
			dst[n] = 0;
			T.file_line_n = 0;
			return 1;
		}
	}
	n = (int)(nl - T.file_line);
	skip = n;
	if (n > 0 && T.file_line[n - 1] == '\r')
		n--;
	if (n >= dstsz)
		n = dstsz - 1;
	memcpy(dst, T.file_line, (unsigned)n);
	dst[n] = 0;
	skip++;
	T.file_line_n -= skip;
	if (T.file_line_n > 0)
		memmove(T.file_line, T.file_line + skip, (unsigned)T.file_line_n);
	T.file_line[T.file_line_n] = 0;
	return 1;
}

static const char *file_after_ints(const char *p, int want)
{
	int i;

	for (i = 0; i < want; i++)
	{
		while (*p == ' ')
			p++;
		if (*p < '0' || *p > '9')
			return 0;
		while (*p >= '0' && *p <= '9')
			p++;
		if (*p && *p != ' ')
			return 0;
	}
	while (*p == ' ')
		p++;
	return p;
}

static const char *file_int1(const char *p, unsigned *out)
{
	unsigned v = 0;

	while (*p == ' ')
		p++;
	if (*p < '0' || *p > '9')
		return 0;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (unsigned)(*p++ - '0');
	*out = v;
	return p;
}

static void file_replay_poll(void)
{
	char line[TM_FILE_LINE];
	unsigned char buf[512];
	int loops, rc, n, kind_t;
	char *p;
	const char *hex;

	if (T.file_done || T.file_wait || T.file_paused)
		return;
	for (loops = 0; loops < 256; loops++)
	{
		if (!T.file_have_rec)
		{
			rc = file_next_line(line, (int)sizeof(line));
			if (rc < 0)
			{
				T.file_done = 1;
				ser("!REPLAY DONE\r\n");
				return;
			}
			if (rc == 0)
			{
				T.file_done = 1;
				term_rx_interpret();
				if (T.need_draw)
					term_draw();
				ser("!REPLAY DONE\r\n");
				return;
			}
			p = line;
			while (*p == ' ' || *p == '\t')
				p++;
			if (!p[0] || p[0] == '#')
				continue;
			if (p[0] != 'R' && p[0] != 'T')
				continue;
			strncpy(T.file_rec, p, sizeof(T.file_rec) - 1);
			T.file_rec[sizeof(T.file_rec) - 1] = 0;
			T.file_have_rec = 1;
			T.file_rec_timed = 0;
			T.file_rec_ms = 0;
			if (file_after_ints(p + 1, 3))
			{
				unsigned v;

				if (file_int1(p + 1, &v))
				{
					T.file_rec_ms = v;
					T.file_rec_timed = 1;
				}
			}
		}
		/* Readable timing: pace records against the log's own ms clock.
		 * file_pct is the speed as a percentage (100 = real time). */
		if (T.file_pct > 0 && T.file_rec_timed)
		{
			unsigned now = mmb_now_ms();

			if (!T.file_have_ms)
			{
				T.file_have_ms = 1;
				T.file_last_ms = T.file_rec_ms;
				T.file_last_at = now;
			}
			else
			{
				unsigned delta =
					T.file_rec_ms > T.file_last_ms
						? T.file_rec_ms - T.file_last_ms
						: 0;
				unsigned target =
					T.file_last_at +
					(unsigned)((double)delta * 100.0 /
						   (double)T.file_pct);

				if (now < target)
					return;
				T.file_last_at = target;
				T.file_last_ms = T.file_rec_ms;
			}
		}
		p = T.file_rec;
		T.file_have_rec = 0;
		kind_t = (p[0] == 'T');
		p++;
		hex = file_after_ints(p, 3);
		if (!hex)
			hex = file_after_ints(p, 2);
		if (!hex)
			continue;
		n = file_hex(hex, buf, (int)sizeof(buf));
		if (!kind_t)
		{
			if (n > 0)
				incoming_feed(buf, n);
			continue;
		}
		if (n <= 0 || buf[0] == IAC)
			continue;
		if (n > TM_FILE_PEND)
			n = TM_FILE_PEND;
		memcpy(T.file_pend, buf, (unsigned)n);
		T.file_pend_n = n;
		T.file_wait = 1;
		term_rx_interpret();
		if (T.need_draw)
			term_draw();
		ser("!REPLAY WAIT\r\n");
		return;
	}
	term_rx_interpret();
	if (T.need_draw)
		term_draw();
}

static void term_replay_speed_msg(void)
{
	ser("!REPLAY SPEED ");
	ser_u((unsigned)T.file_pct);
	if (T.file_paused)
		ser(" PAUSED");
	ser("\r\n");
}

static void demo_iac_tick(void)
{
	static const unsigned char g1[] = {
		'G', '1', ':', IAC, SB, 'K', 'E', 'E', 'P',
		IAC, SE, 'A', IAC, 0x80, 'B', '\r', '\n'
	};
	static const unsigned char g2[] = {
		'G', '2', ':', IAC, WILL, TELOPT_ECHO,
		IAC, DO, TELOPT_SGA, 'O', 'K', '\r', '\n'
	};
	static const unsigned char g3[] = {
		'G', '3', ':', IAC, IAC, 'X', '\r', '\n'
	};
	static const unsigned char g4[] = {
		'G', '4', ':', IAC, SB, TELOPT_TTYPE, TTYPE_SEND, IAC, SE,
		'T', 'Y', '\r', '\n'
	};
	static const unsigned char g5a[] = { 'G', '5', ':', IAC };
	static const unsigned char g5b[] = { 0x80, 'G', 'O', '\r', '\n' };

	if (T.demo_iac_step == 0)
		incoming_feed(g1, (int)sizeof(g1));
	else if (T.demo_iac_step == 1)
		incoming_feed(g2, (int)sizeof(g2));
	else if (T.demo_iac_step == 2)
		incoming_feed(g3, (int)sizeof(g3));
	else if (T.demo_iac_step == 3)
		incoming_feed(g4, (int)sizeof(g4));
	else if (T.demo_iac_step == 4)
		incoming_feed(g5a, (int)sizeof(g5a));
	else if (T.demo_iac_step == 5)
		incoming_feed(g5b, (int)sizeof(g5b));
	else
		return;
	T.demo_iac_step++;
	term_serial_dump();
}

static void tcp_close_quiet(void)
{
	s_iac_n = 0;
	mmb_net_tcp_close();
	mmb_tcp_release();
	T.tcp = 0;
	T.net_lost = 0;
}

/* The socket reported an error or EOF: remember why, keep whatever is
   still in the ring, and let mmb_term_poll() render both. */
static void tcp_lost(const char *why)
{
	unsigned n;

	s_iac_n = 0;
	mmb_net_tcp_close();
	mmb_tcp_release();
	T.tcp = 0;
	strncpy(T.net_msg, "Connection closed", sizeof(T.net_msg) - 1);
	T.net_msg[sizeof(T.net_msg) - 1] = 0;
	if (why && why[0])
	{
		n = (unsigned)strlen(T.net_msg);
		if (n + 2 < sizeof(T.net_msg))
		{
			T.net_msg[n++] = ':';
			T.net_msg[n++] = ' ';
			strncpy(T.net_msg + n, why, sizeof(T.net_msg) - 1 - n);
			T.net_msg[sizeof(T.net_msg) - 1] = 0;
		}
	}
	T.net_lost = 1;
	term_log_event(T.net_msg);
	ser("!NET ");
	ser(T.net_msg);
	ser("\r\n");
	if (T.live)
		term_net_stats(0);
}

static void term_net_lost_report(void)
{
	T.net_lost = 0;
	if (mmb_net_rxbuf_used(&term_rx))
		term_rx_interpret();
	if (T.cur_col != 0)
		pane_newline();
	pane_puts(T.net_msg);
	pane_newline();
	T.net_fail = 1;
	mark_dirty_full();
	term_draw();
	term_serial_dump();
}

static void replay_closed(const char *why)
{
	if (!T.replay || !T.tcp)
		return;
	tcp_lost(why);
	term_net_lost_report();
}

static int swallow_crlf_pair(char c)
{
	if (c != '\r' && c != '\n')
	{
		T.last_eol = 0;
		return 0;
	}
	if (T.last_eol && T.last_eol != (unsigned char)c)
	{
		T.last_eol = 0;
		return 1;
	}
	T.last_eol = (unsigned char)c;
	return 0;
}

static void send_enter(void)
{
	term_net_send("\r", 1);
}

static void send_line(void)
{
	T.line[T.linelen] = 0;
	if (T.linelen)
		term_net_send(T.line, (unsigned)T.linelen);
	if (T.tcp)
		term_net_send("\r", 1);
	else
		term_net_send("\r\n", 2);
	T.linelen = 0;
}

static void esc_reset(void)
{
	T.esc = 0;
	T.csi_n = 0;
	T.esc_len = 0;
}

static void esc_send(void)
{
	if (T.tcp && T.esc_len > 0)
		term_net_send(T.esc_buf, (unsigned)T.esc_len);
	esc_reset();
}

static int esc_feed(char c)
{
	if (T.esc_len + 1 < TM_ESC_BUF)
		T.esc_buf[T.esc_len++] = c;
	if (T.esc == 1)
	{
		if (c == '[' || c == 'O')
		{
			T.esc = (c == '[') ? 2 : 5;
			T.csi_n = 0;
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 2)
	{
		if (c == '[')
		{
			T.esc = 6;
			return 1;
		}
		if (c == 'A' || c == 'B' || c == 'C' || c == 'D')
		{
			if (T.dlg)
			{
				term_dlg_arrow(c);
				esc_reset();
				return 1;
			}
			if (T.menu)
			{
				if (c == 'A' || c == 'B')
					term_menu_move(c == 'A' ? -1 : 1);
				esc_reset();
				return 1;
			}
			/* Up/Down are normal cursor keys: always forwarded to the
			 * session. Only Page Up/Page Down move the scrollback
			 * viewport (#592). */
			esc_send();
			return 1;
		}
		if ((c == 'H' || c == 'F') && !T.dlg && !T.menu && T.sb_view > 0)
		{
			if (c == 'H')
				sb_scroll_by(sb_max_view());
			else
				sb_to_live();
			esc_reset();
			return 1;
		}
		if (c >= '0' && c <= '9')
		{
			T.csi_n = c - '0';
			T.esc = 4;
			return 1;
		}
		if (T.dlg || T.menu)
		{
			esc_reset();
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 4)
	{
		if (c >= '0' && c <= '9')
		{
			T.csi_n = T.csi_n * 10 + (c - '0');
			return 1;
		}
		if (c == '~')
		{
			int page = T.pane_rows > 2 ? T.pane_rows - 2 : 1;

			if (T.csi_n == 21)
			{
				esc_reset();
				term_open_menu();
				return 1;
			}
			if (T.dlg || T.menu)
			{
				esc_reset();
				return 1;
			}
			if (T.csi_n == 5 || T.csi_n == 6)
			{
				if (G.opt.term_scroll_buffer)
				{
					if (T.csi_n == 5)
					{
						if (T.sb_view > 0 || H.n > 0)
							sb_scroll_by(page);
					}
					else if (T.sb_view > 0)
						sb_scroll_by(-page);
					esc_reset();
					return 1;
				}
				esc_send();
				return 1;
			}
			if (T.csi_n == 1 && T.sb_view > 0)
			{
				sb_scroll_by(sb_max_view());
				esc_reset();
				return 1;
			}
			if (T.csi_n == 4 && T.sb_view > 0)
			{
				sb_to_live();
				esc_reset();
				return 1;
			}
			esc_send();
			return 1;
		}
		if (T.dlg || T.menu)
		{
			esc_reset();
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 5)
	{
		if (T.dlg || T.menu)
		{
			esc_reset();
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 6)
	{
		esc_reset();
		return 1;
	}
	return 0;
}

static void demo_emit_line(void)
{
	char buf[64];
	const char *s;

	if (T.demo_line == 0)
		s = "TERM demo";
	else if (T.demo_line == 1)
		s = "Luxurious terminal";
	else if (T.demo_line == 2)
		s = "Alt-X to leave";
	else if (T.demo_line == 3)
		s = "CP437 shades";
	else if (T.demo_line <= 43)
	{
		fmt_line_num(buf, T.demo_line - 3);
		s = buf;
	}
	else
		return;
	pane_puts(s);
	pane_newline();
	term_serial_dump();
	T.demo_line++;
	T.demo_next = mmb_now_ms() + TM_DEMO_MIN_MS +
		(mmb_now_ms() % (TM_DEMO_MAX_MS - TM_DEMO_MIN_MS + 1));
}

static int term_log_name(const char *name)
{
	int n = (int)strlen(name);
	const char *sufs[2];
	int i;

	sufs[0] = ".termlog";
	sufs[1] = ".log";
	for (i = 0; i < 2; i++)
	{
		int m = (int)strlen(sufs[i]);

		if (n >= m && strcasecmp(name + n - m, sufs[i]) == 0)
			return 1;
	}
	return 0;
}

static int term_sessions_dir(const char *dir, int *truncated)
{
	char list[2048];
	char *p;
	int found = 0;

	if (truncated)
		*truncated = 0;
	list[0] = 0;
	if (mmb_vfs_list(dir, list, sizeof(list), truncated) != 0)
		return 0;
	p = list;
	while (*p)
	{
		char name[96];
		int m = 0;

		while (*p && *p != '\n' && *p != '\r' && m < (int)sizeof(name) - 1)
			name[m++] = *p++;
		name[m] = 0;
		while (*p == '\r' || *p == '\n')
			p++;
		if (m <= 0 || name[m - 1] == '/' || !term_log_name(name))
			continue;
		{
			char full[160];
			unsigned fl = 0;
			const char *src = dir;
			const char *nm = name;

			while (*src && fl + 1 < sizeof(full))
				full[fl++] = *src++;
			if (fl > 0 && full[fl - 1] != '/' && fl + 1 < sizeof(full))
				full[fl++] = '/';
			while (*nm && fl + 1 < sizeof(full))
				full[fl++] = *nm++;
			full[fl] = 0;
			mmb_out(full);
			mmb_out(" ");
			mmb_outf(0, (int64_t)mmb_vfs_size(full));
			mmb_out("\n");
			found++;
		}
	}
	return found;
}

static void term_sessions_list(void)
{
	char root[4];
	char def[24];
	int found = 0, sz, trunc1 = 0, trunc2 = 0;

	found += term_sessions_dir(mmb_vfs_cwd(), &trunc1);
	root[0] = mmb_fat_ready('C') ? 'C' : 'A';
	root[1] = ':';
	root[2] = '/';
	root[3] = 0;
	found += term_sessions_dir(root, &trunc2);
	/* The default capture path is a dotfile, so directory listings hide it;
	 * surface it explicitly. */
	term_log_path(def, sizeof(def));
	sz = mmb_vfs_size(def);
	if (sz >= 0)
	{
		mmb_out(def);
		mmb_out(" ");
		mmb_outf(0, (int64_t)sz);
		mmb_out("\n");
		found++;
	}
	if (trunc1 || trunc2)
		mmb_out("... more\n");
	if (!found)
		mmb_out("No saved TERM sessions");
	mmb_out("\n");
}

void mmb_cmd_term(void)
{
	mmb_val host, portv, pathv;
	int port, i, no_args, file_mode, file_sz, file_pct;
	char file_path[88];

	mmb_skip_sp();
	file_mode = 0;
	file_sz = 0;
	file_pct = 0;
	file_path[0] = 0;
	port = 0;
	host = mmb_str_val("");
	no_args = (*G.p == 0 || *G.p == ':' || *G.p == '\'');
	if (mmb_match("SESSIONS"))
	{
		term_sessions_list();
		return;
	}
	if (mmb_match("REPLAY"))
	{
		pathv = mmb_expr();
		if (pathv.type != T_STR)
			mmb_syntax();
		mmb_skip_sp();
		if (*G.p == ',')
		{
			mmb_val sv;

			G.p++;
			mmb_skip_sp();
			sv = mmb_expr();
			if (sv.type != T_INT && sv.type != T_NUM)
				mmb_syntax();
			file_pct = (int)mmb_as_int(sv) * 100;
			if (file_pct < 0)
				file_pct = 0;
			if (file_pct > 6400)
				file_pct = 6400;
		}
		if (mmb_vfs_resolve(pathv.s, file_path, (int)sizeof(file_path)) != 0)
			mmb_error("?FILE");
		file_sz = mmb_vfs_size(file_path);
		if (file_sz < 0)
			mmb_error("?FILE");
		file_mode = 1;
		no_args = 0;
	}
	else if (!no_args)
	{
		host = mmb_expr();
		if (host.type != T_STR)
			mmb_syntax();
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
			mmb_syntax();
		portv = mmb_expr();
		if (portv.type != T_INT && portv.type != T_NUM)
			mmb_syntax();
		port = (int)mmb_as_int(portv);
		if (port < 1 || port > 65535)
			mmb_error("?SYNTAX ERROR");
	}

	if (term_session_alloc() != 0)
		mmb_error("?OUT OF MEMORY");
	term_cache_free(g_console);
	term_apc_buf_free(g_console);
	term_pixbuf_free_all(g_console);
	term_mask_free(g_console);
	memset(&T, 0, sizeof(T));
	H.n = 0;
	H.head = 0;
	mmb_term_scrollback_set(G.opt.term_scrollback);
	s_iac_n = 0;
	zm_setup();
	mmb_zm_forget(&ZM);
	zm_shift = 0;
	term_bm_load();
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	mmb_net_rxbuf_reset(&term_rx);
	if (file_mode)
	{
		strncpy(T.file_path, file_path, sizeof(T.file_path) - 1);
		T.file_path[sizeof(T.file_path) - 1] = 0;
		T.file_sz = file_sz;
		T.file_replay = 1;
		T.file_pct = file_pct;
		T.tcp = 1;
		strncpy(T.host, "file", sizeof(T.host) - 1);
	}
	else if (!no_args)
	{
		strncpy(T.host, host.s, sizeof(T.host) - 1);
		T.host[sizeof(T.host) - 1] = 0;
		T.port = port;
	}
	T.saved_mode = G.gfx.mode;
	T.saved_bits = G.gfx.bits;
	T.demo = host_is_demo();
	T.demo_burst = (strcasecmp(T.host, "demoburst") == 0);
	T.demo_iac = (strcasecmp(T.host, "demoiac") == 0);
	T.replay = !T.file_replay && host_is_replay();
	T.letterbox = 1;
	T.mode80x25 = 0;
	T.sixel_scroll = 1;
	term_reset_pen();

	ser("TERM\r\n");
	if (!T.demo && !T.replay && !T.file_replay)
	{
		if (mmb_tcp_owner() >= 0 && mmb_tcp_owner() != g_console)
			mmb_tcp_in_use();
		if (mmb_tcp_any_open())
			mmb_error("?FILE");
	}
	if (T.replay)
	{
		T.tcp = 1;
		T.mon_ansi = -1;
		T.mon_iac = -1;
		T.mon_sb = -1;
	}
	else if (T.file_replay)
	{
		ser("!REPLAY START\r\n");
		if (T.file_pct > 0)
			term_replay_speed_msg();
	}
	else if (!T.demo && T.host[0])
	{
		if (mmb_net_tcp_begin(T.host, T.port) != 0)
		{
			T.net_fail = 1;
			strncpy(T.net_msg, mmb_net_tcp_errmsg(), sizeof(T.net_msg) - 1);
		}
		else
		{
			mmb_tcp_claim();
			term_mark_live();
			T.connecting = 1;
			T.connect_at = mmb_now_ms();
		}
	}

	if (G.opt.term_autolog && !T.file_replay && !G.opt.term_log)
	{
		G.opt.term_log = 1;
		mmb_term_log_enable(1);
	}

	term_apply_session_mode();
	/* Always wipe Circle console + hide cursor. set_mode's fill_screen is
	 * skipped when mode/bits are unchanged (e.g. already mode 14), which
	 * left the MMBasic prompt on HDMI until the menu covered it (#314). */
	mmb_hw_cursor(0);
	if (G.plat && G.plat->fill_screen)
		G.plat->fill_screen(0);
	G.gfx.write_page = 0;
	G.gfx.display_page = 0;
	mmb_gfx_cls(TM_BG);
	G.gfx.write_page = TM_PAGE;
	mmb_gfx_cls(TM_BG);

	term_layout();
	term_reset_scroll_region();
	for (i = 0; i < T.pane_rows; i++)
		pane_clear_row(i);
	T.cur_row = 0;
	T.cur_col = 0;
	T.cur_vis = 1;
	T.active = 1;
	/* #858: keep a live session drained if this console is switched away
	 * from. The callback itself checks the socket owner, so demo/replay/file
	 * sessions (which have no socket) are not touched. */
	mmb_yield_remove(term_bg_yield);
	mmb_yield_add(g_console, term_bg_yield, 0, 25);
	T.dirty_lo = -1;
	T.dirty_hi = -1;
	mark_dirty_full();

	if (T.replay)
	{
		pane_puts("Connected");
		pane_newline();
	}
	else if (T.file_replay)
	{
		pane_puts("Replay");
		pane_newline();
	}
	if (T.net_fail && T.net_msg[0])
	{
		pane_puts(T.net_msg);
		pane_newline();
	}
	else if (T.connecting)
	{
		pane_puts("Connecting...");
		pane_newline();
	}
	else if (no_args)
	{
		pane_puts("Disconnected - F10 for Bookmarks");
		pane_newline();
	}
	else if (T.demo && !T.demo_iac)
	{
		int n = T.demo_burst ? 40 : 2;
		T.demo_next = mmb_now_ms();
		while (n > 0 && T.demo_line <= 43)
		{
			demo_emit_line();
			n--;
		}
	}

	term_draw();
	if (!T.file_replay)
		term_serial_dump();
	if (T.tcp && !T.file_replay)
		telnet_announce();
	else if (T.file_replay)
		file_replay_poll();
}

int mmb_in_term(void)
{
	return T.active;
}

const char *mmb_term_key(char c)
{
	unsigned char b;

	if (!T.active)
		return "";
	if (T.file_wait && (unsigned char)c != 1 && !T.alt)
	{
		T.file_pend_n = 0;
		T.file_wait = 0;
		file_replay_poll();
		if (T.need_draw)
			term_draw();
		return "";
	}
	if (T.file_replay && !T.alt)
	{
		if (c == '+' || c == '=')
		{
			T.file_pct = T.file_pct > 0 ? T.file_pct * 2 : 100;
			if (T.file_pct > 6400)
				T.file_pct = 6400;
			term_replay_speed_msg();
			return "";
		}
		if (c == '-' || c == '_')
		{
			T.file_pct = T.file_pct > 100 ? T.file_pct / 2 : (T.file_pct > 0 ? 50 : 100);
			term_replay_speed_msg();
			return "";
		}
		if (c == ' ')
		{
			T.file_paused = !T.file_paused;
			term_replay_speed_msg();
			return "";
		}
	}
	if (T.replay && replay_key(c))
	{
		if (T.need_draw)
			term_draw();
		replay_mon();
		return "";
	}
	if (mmb_zm_active(&ZM))
	{
		/* A transfer owns the connection: keystrokes are not forwarded.
		 * Esc aborts the download, Alt-X aborts and exits. */
		if ((unsigned char)c == 1)
		{
			T.alt = 1;
			T.alt_pend = 1;
			return "";
		}
		if (T.alt)
		{
			T.alt = 0;
			T.alt_pend = 0;
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			mmb_zm_cancel(&ZM);
			if (c == 'x')
				term_exit();
			return "";
		}
		if (c == 27 || c == 'x' || c == 'X')
			mmb_zm_cancel(&ZM);
		return "";
	}
	if (T.sb_search)
	{
		if (c == 27)
		{
			T.sb_search = 0;
			sb_refresh();
			return "";
		}
		if (c == '\r' || c == '\n')
		{
			sb_search_run();
			return "";
		}
		if (c == 8 || c == 127)
		{
			if (T.sb_qn > 0)
				T.sb_qn--;
			T.sb_query[T.sb_qn] = 0;
			sb_refresh();
			return "";
		}
		if ((unsigned char)c >= 32 && (unsigned char)c < 127 &&
		    T.sb_qn + 1 < TM_SB_QUERY)
		{
			T.sb_query[T.sb_qn++] = c;
			T.sb_query[T.sb_qn] = 0;
			sb_refresh();
		}
		return "";
	}
	if (T.sb_view > 0 && !T.alt && !T.dlg && !T.menu && !T.esc && c != 27)
	{
		if (c == 'q' || c == 'Q')
		{
			sb_to_live();
			return "";
		}
		if (c == '/')
		{
			sb_search_begin();
			return "";
		}
		if ((c == 'n' || c == 'N') && T.sb_query[0])
		{
			sb_search_step(c == 'n' ? -1 : 1);
			return "";
		}
		/* Any other key jumps back to the live view first. */
		sb_to_live();
	}
	if ((unsigned char)c != 1)
	{
		unsigned char kb = (unsigned char)c;
		if (!T.alt && !T.esc && !T.dlg && !T.menu &&
		    (T.tcp || T.demo) && swallow_crlf_pair(c))
			return "";
		if (T.tcp && (kb == '\n' || kb == '\r'))
			kb = '\r';
		term_log_tx(&kb, 1);
	}
	if (T.alt)
	{
		T.alt = 0;
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c == 'x')
		{
			term_exit();
			return "";
		}
		if (c == 't' || c == 'f')
		{
			term_open_menu();
			return "";
		}
		if (T.dlg == TM_DLG_EDIT)
		{
			if (c == 'c')
			{
				term_dlg_close();
				return "";
			}
			if (c == 's')
			{
				term_bm_save_edit();
				return "";
			}
		}
		T.alt_pend = 0;
		if (!T.menu)
			term_close_menu();
		return "";
	}
	if ((unsigned char)c == 1)
	{
		/* Connected to a remote session with no local menu/dialog open:
		 * Ctrl+A is the screen/tmux prefix, so forward it instead of
		 * showing TERM's chrome. A real Alt key still takes the local
		 * path (menu/exit), and F10 is the menu key when no Alt exists
		 * (#1218). */
		if (T.tcp && !T.file_replay && !T.dlg && !T.menu &&
		    T.sb_view <= 0 && !term_alt_held())
		{
			unsigned char ca = 1;

			term_log_tx(&ca, 1);
			term_net_send(&ca, 1);
			return "";
		}
		T.alt = 1;
		T.alt_pend = 1;
		term_overlay_chrome();
		return "";
	}
	if (c == 27)
	{
		if (T.esc == 1)
		{
			if (T.dlg)
			{
				term_dlg_close();
				esc_reset();
				return "";
			}
			if (T.menu)
			{
				esc_reset();
				term_close_menu();
				return "";
			}
			if (T.esc_len <= 0)
			{
				T.esc_buf[0] = 27;
				T.esc_len = 1;
			}
			esc_send();
		}
		T.esc = 1;
		T.esc_len = 0;
		T.esc_buf[0] = 27;
		T.esc_len = 1;
		T.esc_at = mmb_now_ms();
		return "";
	}
	if (T.esc)
	{
		if (esc_feed(c))
		{
			if (!T.active)
				return "";
			return "";
		}
	}
	if (T.dlg)
	{
		term_dlg_key(c);
		return "";
	}
	if (T.menu)
	{
		if (c == '\r' || c == '\n')
		{
			term_menu_activate();
			return "";
		}
		if (c == 'x' || c == 'X')
		{
			term_exit();
			return "";
		}
		if (c == 'e' || c == 'E')
		{
			term_toggle_echo();
			return "";
		}
		if (c == 'w' || c == 'W' || c == 'o' || c == 'O')
		{
			term_cycle_view();
			return "";
		}
		if (c == 's' || c == 'S')
		{
			term_toggle_scroll_buffer();
			return "";
		}
		if (c == 'b' || c == 'B' || c == 'k' || c == 'K')
		{
			term_bm_open_list();
			return "";
		}
		return "";
	}
	if (!T.tcp && !T.demo)
		return "";
	if (T.char_mode)
	{
		if (c == '\r' || c == '\n')
		{
			if (T.tcp)
				send_enter();
			term_echo_byte('\n');
			term_echo_flush();
			return "";
		}
		b = (unsigned char)c;
		/* A USB Backspace arrives as DEL (127). Linux getty/login sets
		 * stty erase to ^?, so sending BS (^H) over telnet prints the
		 * literal caret sequence instead of erasing (#1255). Keep BS
		 * for local/demo sessions that expect it. */
		if (b == 127 && !T.tcp)
			b = 8;
		if (T.tcp)
		{
			if (b == IAC)
			{
				unsigned char esc[2] = { IAC, IAC };
				term_net_send(esc, 2);
			}
			else
				term_net_send(&b, 1);
		}
		term_echo_byte(b);
		term_echo_flush();
		return "";
	}
	if (c == '\r' || c == '\n')
	{
		if (T.tcp)
			send_line();
		else
			T.linelen = 0;
		term_echo_byte('\n');
		term_echo_flush();
		return "";
	}
	if (c == 8 || c == 127)
	{
		if (T.linelen > 0)
		{
			T.linelen--;
			term_echo_byte(8);
			term_echo_flush();
		}
		return "";
	}
	if ((unsigned char)c < 32)
		return "";
	if (T.linelen + 1 < TM_LINE)
	{
		T.line[T.linelen++] = c;
		term_echo_byte((unsigned char)c);
		term_echo_flush();
	}
	return "";
}

void mmb_term_poll(void)
{
	int got;

	if (!T.active)
		return;
	term_log_poll();
	if (mmb_zm_active(&ZM))
	{
		zmodem_poll();
		return;
	}
	if (ZM.state != MMB_ZM_IDLE)
	{
		zmodem_finished();
		return;
	}
	if (T.esc == 1 && T.esc_at &&
	    mmb_now_ms() - T.esc_at >= TM_ESC_IDLE_MS)
	{
		if (T.dlg)
		{
			term_dlg_close();
			esc_reset();
		}
		else if (T.menu)
		{
			esc_reset();
			term_close_menu();
		}
		else
		{
			if (T.esc_len <= 0)
			{
				T.esc_buf[0] = 27;
				T.esc_len = 1;
			}
			esc_send();
		}
	}
	if (T.demo_iac)
		demo_iac_tick();
	else if (T.demo && T.demo_line <= 43 && mmb_now_ms() >= T.demo_next)
		demo_emit_line();
	if (T.file_replay)
	{
		file_replay_poll();
		if (T.need_draw)
			term_draw();
		return;
	}
	if (T.replay)
	{
		term_rx_interpret();
		if (T.need_draw)
			term_draw();
		ansi_watchdog();
		sb_watchdog();
		if (T.need_draw)
			term_draw();
		replay_mon();
		return;
	}
	if (T.connecting)
	{
		int st = mmb_net_tcp_status();
		if (st == 0 && mmb_now_ms() - T.connect_at < TM_CONNECT_MS)
		{
			if (mmb_net_tcp_cancelling() &&
			    mmb_now_ms() - T.connect_at >= 400)
			{
				mark_dirty_full();
				term_draw();
			}
			return;
		}
		T.connecting = 0;
		if (st == 1)
		{
			T.tcp = 1;
			T.cur_vis = 1;
			telnet_announce();
			pane_puts("Connected");
			pane_newline();
			term_draw();
			if (T.live)
				term_net_stats("connected");
			else
				term_serial_dump();
		}
		else
		{
			if (mmb_now_ms() - T.connect_at >= TM_CONNECT_MS && st == 0)
				strncpy(T.net_msg, "TCP timeout", sizeof(T.net_msg) - 1);
			else
				strncpy(T.net_msg, mmb_net_tcp_errmsg(),
					sizeof(T.net_msg) - 1);
			mmb_net_tcp_close();
			mmb_tcp_release();
			T.net_fail = 1;
			pane_puts(T.net_msg);
			pane_newline();
			term_draw();
			if (T.live)
			{
				ser("!NET fail ");
				ser(T.net_msg);
				ser("\r\n");
				term_net_stats(0);
			}
			else
				term_serial_dump();
			return;
		}
	}
	if (!T.tcp)
	{
		if (T.net_lost)
			term_net_lost_report();
		if (T.need_draw)
			term_draw();
		return;
	}
	got = term_tcp_ingest(TM_RECV_IDLE);
	if (got < 0)
		return;
	term_maybe_serial_dump(got);
	if (T.need_draw)
		term_draw();
	got = term_tcp_ingest(TM_RECV_IDLE);
	if (got < 0)
		return;
	term_maybe_serial_dump(got);
	if (T.need_draw)
		term_draw();
	term_yield();
}

/* #858: keep an established TCP session drained while this console is not the
 * active screen. Received bytes are interpreted into this console's own grid
 * (scrollback, log, ANSI state all stay correct); painting is suppressed by
 * s_term_bg and the active poll repaints from need_draw on return. Only the
 * console that owns the machine-wide socket has a real session, so the shared
 * receive ring has a single producer. Key handling never runs here. */
static void term_bg_yield(int console, void *ctx)
{
	(void)ctx;
	if (console == g_console)
		return;
	if (!T_s[console].active)
		return;
	if (mmb_tcp_owner() != console)
		return;
	if (!mmb_bg_console_enter(console))
		return;
	s_term_bg = 1;
	mmb_term_poll();
	s_term_bg = 0;
	mmb_bg_console_leave();
}

/* Cold-boot the TERM layer on a warm reset (#763). Closing each console's
 * session is done by warm_reset_close_apps(); this clears every console's
 * log, scrollback, bookmarks and download/browser state that would otherwise
 * survive, and frees the lazily allocated log/scrollback rings (#767). */
void mmb_term_reset_all(void)
{
	int i;

	for (i = 0; i < MMB_MAX_CONSOLES; i++)
	{
		memset(&T_s[i], 0, sizeof(T_s[i]));
		term_session_free(i);
	}
	memset(g_dl_dir_s, 0, sizeof(g_dl_dir_s));
	memset(g_dl_cur_s, 0, sizeof(g_dl_cur_s));
	memset(g_dl_names_s, 0, sizeof(g_dl_names_s));
	memset(g_dl_n_s, 0, sizeof(g_dl_n_s));
	memset(g_dl_sel_s, 0, sizeof(g_dl_sel_s));
	memset(g_dl_top_s, 0, sizeof(g_dl_top_s));
	memset(g_dl_focus_s, 0, sizeof(g_dl_focus_s));
	memset(g_dl_trunc_s, 0, sizeof(g_dl_trunc_s));
	memset(g_dl_last_name_s, 0, sizeof(g_dl_last_name_s));
	memset(g_dl_last_files_s, 0, sizeof(g_dl_last_files_s));
	memset(g_bm_s, 0, sizeof(g_bm_s));
	memset(g_bm_n_s, 0, sizeof(g_bm_n_s));
	memset(dlg_c0_s, 0, sizeof(dlg_c0_s));
	memset(dlg_r0_s, 0, sizeof(dlg_r0_s));
	memset(dlg_cw_s, 0, sizeof(dlg_cw_s));
	memset(dlg_ch_s, 0, sizeof(dlg_ch_s));
	memset(s_iac_out_s, 0, sizeof(s_iac_out_s));
	memset(s_iac_n_s, 0, sizeof(s_iac_n_s));
	memset(ZM_s, 0, sizeof(ZM_s));
	memset(zm_ready_s, 0, sizeof(zm_ready_s));
	memset(zm_shift_s, 0, sizeof(zm_shift_s));
	memset(zm_shown_s, 0, sizeof(zm_shown_s));
	memset(zm_cur_path_s, 0, sizeof(zm_cur_path_s));
}
