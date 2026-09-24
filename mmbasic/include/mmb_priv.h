#ifndef MMB_PRIV_H
#define MMB_PRIV_H

#include "mmbasic.h"
#if defined(MMB_PLATFORM_POSIX)
/* Native (Linux/macOS) build: use libc for setjmp/alloc/strings. This is a
 * compile-time-only substitution so the Circle build keeps its own headers
 * and codegen unchanged. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#else
#include <circle/setjmp.h>
#include <circle/alloc.h>
#include <circle/util.h>
#endif
#include <stdint.h>
#include <stddef.h>

#define MMB_MAX_NAME      40
#define MMB_MAX_VARS      256
#define MMB_MAX_DIMS      4
#define MMB_MAX_LINES     2048
#define MMB_LINE_LEN      256
#define MMB_MAX_FILES     10
#define MMB_MAX_PAGES     8
#define MMB_OPT_DEFAULT_MODE       11 /* 1280x720 */
#define MMB_OPT_DEFAULT_PROMPT     1  /* CWD */
#define MMB_OPT_DEFAULT_EDIT_THEME 5  /* Slate */
/* Prompt/console: grey on black (CMM2 is white/black). IBM 15/1 is white/blue. */
#define MMB_DEFAULT_FG             0x808080u
#define MMB_DEFAULT_BG             0u
#define MMB_MAX_BLIT      64
#define MMB_MAX_SPRITE    64
#define MMB_MAX_SUB_ARGS  16
#define MMB_MAX_LOCALS    64
#define MMB_MAX_TICK      4
#define MMB_INKEY         64
#define MMB_FB_MAX_W      1920
#define MMB_FB_MAX_H      1080
#define MMB_PAGE_CUR      (-1)
#define MMB_PAGE_FB       (-2)
#define MMB_TURTLE_MAX    128
#define MMB_OUT_LEN       4096
#define MMB_ED_TABS       6
#define MMB_ED_BUF        16384
/* Shared undo policy for the creative/writing apps (#532): Ctrl+Z is the
 * documented chord and snapshot-based apps keep this many steps. EDIT uses a
 * deeper op-journal; matching UX matters more than identical engines. */
#define MMB_UNDO_DEPTH    8
#define MMB_PROG_NAME     80
#define MMB_MAX_GOSUB     32
#define MMB_MAX_CTRL      32
#define MMB_MAX_CONST     256
#define MMB_MAX_SUBS      128
#define MMB_MAX_LABELS    64

#define T_NUM   1
#define T_INT   2
#define T_STR   4
#define T_STRUCT 8

#define MMB_MAX_STRUCT_TYPES    32
#define MMB_MAX_STRUCT_MEMBERS  16
#define MMB_MAX_STRUCT_NEST     8
#define MMB_STRUCT_STR_DEFAULT  1024
#define MMB_STRUCT_STRLEN       4
#define MMB_STRUCT_RET_MAX      65536

/* Struct STRING members carry a 32-bit length followed by the characters. */
#define MMB_STRUCT_STRLEN_PUT(p, n) \
	do { \
		(p)[0] = (unsigned char)(n); \
		(p)[1] = (unsigned char)((n) >> 8); \
		(p)[2] = (unsigned char)((n) >> 16); \
		(p)[3] = (unsigned char)((n) >> 24); \
	} while (0)
#define MMB_STRUCT_STRLEN_GET(p) \
	((int)((p)[0] | ((p)[1] << 8) | ((p)[2] << 16) | ((unsigned)(p)[3] << 24)))

typedef struct mmb_val {
	int type;          /* T_NUM, T_INT, T_STR, T_STRUCT */
	double f;
	int64_t i;
	const char *s;
	unsigned char *blob;
	int struct_idx;
} mmb_val;

typedef struct mmb_smem {
	char name[MMB_MAX_NAME];
	int type;
	int size;
	int offset;
	int dims;
	int dim[MMB_MAX_DIMS];
	int count;
} mmb_smem;

typedef struct mmb_sdef {
	char name[MMB_MAX_NAME];
	int nmem;
	mmb_smem mem[MMB_MAX_STRUCT_MEMBERS];
	int total;
	int used;
} mmb_sdef;

typedef struct mmb_var {
	char name[MMB_MAX_NAME];
	int type;
	int dims;
	int dim[MMB_MAX_DIMS]; /* inclusive upper bound */
	int size;              /* element count */
	int struct_idx;
	union {
		double *f;
		int64_t *i;
		char **s;
		unsigned char *blob;
	} data;
	int used;
	int unsuffixed; /* 1 = DIM INTEGER N / A=1; 0 = A% / A$ */
	int maxlen;     /* DIM ... LENGTH n: hard string cap (0 = unbounded) */
} mmb_var;

/* One LOCAL declaration in the current SUB/FUNCTION frame: the binding that
 * was in place before the LOCAL shadowed it, so it can be restored on return.
 * `existed == 0` means the variable did not exist before and is removed. */
typedef struct mmb_localsave {
	int slot;
	int existed;
	int type;
	int dims;
	int dim[MMB_MAX_DIMS];
	int size;
	int struct_idx;
	int unsuffixed;
	int maxlen;
	void *data;
} mmb_localsave;

typedef struct mmb_arrview {
	mmb_var *v;
	int moff;
	int mtype;
	int count;
} mmb_arrview;

#ifdef MMB_CIRCLE_WLAN
#define MMB_DEFAULT_CONSOLE 2 /* SCREEN on hardware so TUI does not stall UART */
#else
#define MMB_DEFAULT_CONSOLE 3 /* BOTH in QEMU so pytest can drive the prompt */
#endif

typedef struct mmb_options {
	int base;              /* 0 or 1 */
	int explicit;          /* OPTION EXPLICIT */
	int default_type;      /* T_NUM/T_INT/T_STR/0=NONE */
	int angle_degrees;
	int y_axis_up;
	int tab;
	int break_key;         /* ASCII, 3 = Ctrl-C */
	int autorun;
	int colourcode;
	int colourcode_reverse;
	int console;           /* 1 serial, 2 screen, 3 both, 0 none */
	int console_port;
	int console_saved;
	int crlf;              /* 0 CR, 1 LF, 2 CRLF */
	int default_mode;
	int baudrate;
	int case_mode;         /* 0 UPPER 1 LOWER 2 TITLE */
	int legacy;
	int milliseconds;
	int mouse;
	int mouse_sens;
	int pin;
	int profiling;
	int tracecache;
	int ram_prog;
	int status;
	int vcc_mv;            /* millivolts * 10? store as float via vcc */
	double vcc;
	int sleep_min;
	int sd_fast;
	int serial_pullup;
	int rtc_cal;
	int ds3231;
	int baseline;
	int flash_page;
	int keyboard_lang;     /* 0 US 1 UK 2 DE 3 FR 4 ES */
	int keyboard_noled;
	int repeat_first;
	int repeat_next;
	int edit_font;         /* 0 small .. 4 very large */
	int edit_theme;        /* editor colour theme, default Slate */
	int edit_jump_break;   /* jump to the line on a run break/error */
	int escape;
	char search_path[128];
	char app_path[128];    /* OPTION PATH: .APP dirs, ';' separated (#520) */
	int boot_mode;         /* OPTION BOOT: 0 REPL, 1 launcher, 2 app (#515) */
	char boot_app[80];     /* OPTION BOOT "name": app to run at power-on */
	char fkey[12][65];     /* F1..F12 */
	int list_changed_only;
	int error_continue;    /* 0 ABORT (default) 1 CONTINUE */
	char wifi_ssid[64];
	char wifi_psk[64];
	int wifi_enabled;
	int wifi_debug;        /* OPTION WIFI DEBUG ON|OFF (default OFF) */
	char wifi_country[4];  /* ISO 3166-1 alpha-2 Circle accepts; default US */
	int ethernet_enabled;  /* OPTION ETHERNET ON|OFF (default OFF) */
	int audio_on;          /* OPTION AUDIO ON|OFF (default ON) */
	int audio_target;      /* 0 JACK, 1 HDMI (default HDMI) */
	int prompt;            /* 0 BARE "> ", 1 CWD "A:/> " (default CWD) */
	int term_log;          /* OPTION TERM LOG ON|OFF (default OFF) */
	int term_scrollback;   /* OPTION TERM SCROLLBACK lines (default 200) */
	int term_autolog;      /* OPTION TERM AUTOLOG ON|OFF (default OFF) */
	char ntp_server[64];   /* OPTION NTP SERVER "host[:port]" (#524) */
	int ntp_enabled;       /* OPTION NTP: MMB_NTP_AUTO/0 OFF/1 ON (#581) */
	char timezone[64];     /* OPTION TIMEZONE name/offset (default UTC) */
	int tz_offset_min;     /* derived minutes east of UTC */
} mmb_options;

#define MMB_NTP_DEFAULT_SERVER "pool.ntp.org"
#define MMB_NTP_DEFAULT_PORT   123
/* Default policy: sync at boot once the network is up, unless explicitly
 * disabled with OPTION NTP OFF. */
#define MMB_NTP_AUTO           (-1)

/* Default OPTION PATH: first-party .APP packages ship on the ramdisk (#520). */
#define MMB_APP_PATH_DEFAULT "A:/APPS/"

#define MMB_FK_FILE 0
#define MMB_FK_TCP  1
#define MMB_FM_BOTH 3

typedef struct mmb_file {
	int open;
	int mode; /* 0 input 1 output 2 append 3 both (TCP default) */
	int pos;
	int kind;  /* MMB_FK_FILE or MMB_FK_TCP */
	int ungot; /* TCP pushback, -1 none */
	char path[128];
} mmb_file;

typedef struct mmb_blit_buf {
	int used;
	int w, h;
	uint32_t *pix; /* user-order RGB888 */
} mmb_blit_buf;

typedef struct mmb_gfx {
	int mode;
	int bits;
	int w, h;
	int pages;
	int write_page;
	int display_page;
	int write_fb;          /* drawing goes to framebuffer */
	int fb_w, fb_h;
	/* HDMI-native pixels (16-bit COLOR16 when DEPTH=16). */
	uint16_t *fb;
	uint16_t *fb_bak;
	mmb_blit_buf blit[MMB_MAX_BLIT];
	struct {
		int used, vis, x, y, layer, w, h;
		int seq; /* SPRITE SHOW sequence, for stable equal-layer ordering */
		int next_x, next_y, has_next;
		uint32_t *pix; /* RGB888 + alpha source */
		uint16_t *npix; /* HDMI-native; 0 = transparent */
		uint16_t *store; /* saved background, native */
		uint8_t *astore; /* page-1 alpha snapshot, or NULL */
	} sprite[MMB_MAX_SPRITE];
	int turtle_on;
	double turtle_x, turtle_y, turtle_hdg;
	int turtle_pen;
	unsigned turtle_pen_col, turtle_fill_col;
	int turtle_filling;
	int turtle_fn;
	int turtle_fx[MMB_TURTLE_MAX];
	int turtle_fy[MMB_TURTLE_MAX];
	unsigned fg, bg;
	int font, font_scale;
	/* HDMI-native page buffers. Page 1 alpha lives in page1_alpha. */
	uint16_t *page[MMB_MAX_PAGES];
	uint8_t *page1_alpha; /* 0=clear, 1..15=blend, 255=opaque (black-transparent if 0 colour) */
	int page1_alpha_used; /* any partial AFLAG (1..15) written since last CLS */
	int page1_any;        /* any non-zero native pixel on page 1 since last CLS */
	uint16_t *present_scratch;
	/* Dirty AABB for present_rect coalescing (x0,y0 inclusive; x1,y1 exclusive). */
	int dirty;
	int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
} mmb_gfx;

typedef struct mmb_ed_hop {
	int kind; /* 0 insert, 1 delete */
	int pos;
	int len;
	int off;
	int txn;
} mmb_ed_hop;

typedef struct mmb_ed_hist {
	mmb_ed_hop ops[128];
	int n;          /* ops stored (applied + redo tail) */
	int cur;        /* applied op count */
	int pool_n;
	int saved;      /* cur at last save; -1 = dirty */
	int txn;        /* next txn id */
	int typing;     /* last recorded op was a typing run */
	int typing_txn;
	unsigned typing_at;
	char pool[65536];
} mmb_ed_hist;

typedef struct mmb_ed_tab {
	int used;
	char path[128];
	char buf[MMB_ED_BUF];
	int len;
	int cx, cy, row0, col0;
	int dirty;
	int sel;
	int sel_anchor;
	mmb_ed_hist hist;
} mmb_ed_tab;

typedef struct mmb_editor {
	int active;
	int run_on_exit;
	int wait_continue;
	int saved_mode;
	int saved_bits;
	int saved_write_page;
	int saved_display_page;
	int saved_write_fb;
	int ntabs;
	int cur;
	int menu_open;     /* dropdown visible */
	int menu;          /* 0 File 1 Edit 2 Run 3 Theme 4 Help */
	int menu_item;
	int dialog;        /* 0 none 1 open 2 saveas 3 help */
	char dlg[128];
	int dlglen;
	char status[80];
	mmb_ed_tab tab[MMB_ED_TABS];
} mmb_editor;

typedef struct mmb_audio {
	int playing; /* 0 none 1 mp3 2 mod 3 xm 4 tone */
	int paused;
	int vol_l, vol_r;
	unsigned samples_decoded;
	char name[128];
} mmb_audio;

typedef struct mmb {
	const mmb_platform *plat;
	jmp_buf errjmp;
	char err[160];
	char out[MMB_OUT_LEN];
	int outn;
	const char *p;
	mmb_options opt;
	mmb_var vars[MMB_MAX_VARS];
	int nvars;
	int dim_used;
	mmb_sdef sdef[MMB_MAX_STRUCT_TYPES];
	int nstruct;
	int acc_on;
	int acc_mtype;
	int acc_moff;
	int acc_msize;
	int acc_sid;
	int acc_base_nidx;
	int acc_base_idx[MMB_MAX_DIMS];
	unsigned char func_ret_blob[MMB_STRUCT_RET_MAX];
	char prog[MMB_MAX_LINES][MMB_LINE_LEN];
	int prog_num[MMB_MAX_LINES];
	int nprog;
	char current_prog[MMB_PROG_NAME];
	int running;
	int run_suspended;      /* virtual-console switch suspended RUN */
	int for_sp;
	struct {
		char var[MMB_MAX_NAME];
		mmb_var *vp;
		int off;
		int64_t to, step;
		int line, stmt;
	} forstack[16];
	mmb_gfx gfx;
	mmb_file files[MMB_MAX_FILES + 1];
	mmb_editor ed;
	char cwd[128];
	int drive;             /* 'A'..'H', default 'A' (ramdisk) */
	/* Per-console working directory. The VFS node table and the FAT mount
	 * table are machine-global, but where each console is looking is session
	 * state, so it lives here rather than in file-scope statics. */
	int cwd_node[2];                   /* A: (0) / B: (1) ramdisk node */
	char cwd_path[MMB_MAX_DRIVES][128]; /* C:.. physical, letter - 'A' */
	int data_line, data_pos;
	int gosub_sp;
	int gosub_stack[MMB_MAX_GOSUB];
	int gosub_event[MMB_MAX_GOSUB];
	int gosub_nsave[MMB_MAX_GOSUB];
	char gosub_saven[MMB_MAX_GOSUB][MMB_MAX_SUB_ARGS][MMB_MAX_NAME];
	mmb_val gosub_savev[MMB_MAX_GOSUB][MMB_MAX_SUB_ARGS];
	mmb_val func_ret;
	int branch_pc;         /* GOTO/GOSUB/RETURN/loop control */
	int run_pc;            /* current program line index */
	int on_error_pc;       /* ON ERROR GOTO target, -1 when inactive */
	int error_active;      /* handling a trapped runtime error */
	int err_resume_pc;     /* line that raised the trapped error */
	jmp_buf run_errjmp;    /* setjmp target while a handler is active */
	int ctrl_sp;
	struct {
		int type;          /* 1=while 2=do 3=if 4=select */
		int line_pc;
		int skip;          /* skip statements on this line */
		int endif_pc;      /* for if/select */
	} ctrlstack[MMB_MAX_CTRL];
	int if_skip;           /* multiline IF: skip statements */
	int if_taken;          /* multiline IF: true branch taken */
	int sel_active;
	int sel_skip;
	mmb_val sel_val;
	char *sel_str;
	char *func_ret_s;
	char *gosub_ss[MMB_MAX_GOSUB][MMB_MAX_SUB_ARGS];
	int gosub_nlocal[MMB_MAX_GOSUB];
	mmb_localsave gosub_local[MMB_MAX_GOSUB][MMB_MAX_LOCALS];
	int gosub_sel_skip[MMB_MAX_GOSUB];
	int gosub_sel_active[MMB_MAX_GOSUB];
	int gosub_if_skip[MMB_MAX_GOSUB];
	int gosub_if_taken[MMB_MAX_GOSUB];
	mmb_val gosub_sel_val[MMB_MAX_GOSUB];
	char *gosub_sel_str[MMB_MAX_GOSUB];
	int nconst;
	struct {
		char name[MMB_MAX_NAME];
		int type;
		mmb_val val;
		char *s;
		int used;
	} consts[MMB_MAX_CONST];
	int nsubs;
	struct {
		char name[MMB_MAX_NAME];
		int line_pc;
		int is_func;
		int used;
		int nargs;
		int ret_sid;
		int ret_type; /* T_NUM/T_INT/T_STR return type for functions */
		int arg_sid[MMB_MAX_SUB_ARGS];
		int arg_type[MMB_MAX_SUB_ARGS]; /* declared scalar type, 0 = from name */
		char args[MMB_MAX_SUB_ARGS][MMB_MAX_NAME];
	} subs[MMB_MAX_SUBS];
	int in_sub;            /* executing inside sub body */
	int home_prompt;       /* CLS: next immediate prompt has no leading CR/LF */
	int quit_requested;    /* QUIT: host app should exit its main loop */
	int print_x;           /* PRINT/LOCATE cursor X in pixels */
	int print_y;           /* PRINT/LOCATE cursor Y in pixels */
	int print_locate;      /* next PRINT emits cursor positioning first */
	int64_t timer_base;    /* TIMER = n  →  TIMER reports now-ms minus this */
	uint32_t rnd_seed;
	char date_s[16];
	char time_s[16];
	int clk_y, clk_mo, clk_d, clk_h, clk_mi, clk_s;
	unsigned clk_ms;
	int dim_local;
	char on_key[MMB_MAX_NAME];
	int tick_busy;
	struct {
		int period;
		char sub[MMB_MAX_NAME];
		unsigned last;
	} tick[MMB_MAX_TICK];
	int inkey_q[MMB_INKEY];
	int inkey_r, inkey_w, inkey_n;
	int keydown[6];
	int nkeydown;
	int nlabels;
	struct {
		char name[MMB_MAX_NAME];
		int pc;
		int used;
	} labels[MMB_MAX_LABELS];
	struct {
		unsigned t0_ms;
		unsigned stmt;
		unsigned match;
		unsigned expr;
		unsigned find_var;
		unsigned check_break;
		unsigned gfx_present;
		unsigned tcache_hit;
		unsigned tcache_comp;
		unsigned tcache_bad;
	} prof;
	/* Per-console interpreter run state (formerly file statics in core.c)
	 * so multiple interpreter contexts can suspend and resume. */
	int jmp_wend[MMB_MAX_LINES];
	int jmp_loop[MMB_MAX_LINES];
	int jmp_next[MMB_MAX_LINES];
	int jmp_endsub[MMB_MAX_LINES];
	int jmp_else[MMB_MAX_LINES];
	int jmp_endif[MMB_MAX_LINES];
	int jmp_endsel[MMB_MAX_LINES];
	int jmp_ready;
	int run_preserve_vars;
} mmb;

/* Virtual consoles: one interpreter context per console. The active context
 * is selected by the session manager, so all existing interpreter code keeps
 * using G unchanged. Contexts are heap-allocated (each is ~2 MB) to stay
 * inside Circle's kernel BSS budget. MMB_MAX_CONSOLES lives in mmbasic.h. */
extern mmb *g_mmb[MMB_MAX_CONSOLES];
extern mmb *g_cur;
extern int g_console;
#define G (*g_cur)

/* The audio engine is a single machine resource shared by every console, so
 * its status is global rather than a field of the per-console mmb. */
extern mmb_audio g_audio;

/* Virtual-console suspension handshake (session.c / core.c). */
int mmb_console_switch_pending(void);
int mmb_program_suspended(void);
void mmb_resume_program(void);

void mmb_error(const char *msg);
void mmb_syntax(void);
void mmb_skip_sp(void);
int mmb_normalize_newlines(char *buf, int len);
int mmb_match(const char *kw);
int mmb_match_exact(const char *kw);
void mmb_expect(char c);
int mmb_is_ident(char c);
int mmb_is_digit(char c);
void mmb_ident(char *dst, int dstsz);
int mmb_type_suffix(char *name); /* strips $ % ! and returns type, 0 if none */
int sprintf(char *str, const char *fmt, ...);
mmb_val mmb_expr(void);
mmb_val mmb_num_val(double f);
mmb_val mmb_int_val(int64_t i);
mmb_val mmb_str_val(const char *s);
mmb_val mmb_str_valn(const char *s, int n);
mmb_val mmb_arena_val(char *p);
void mmb_str_reset(void);
void mmb_strpool_reset(void);
char *mmb_tmp_alloc(int n);
char *mmb_str_alloc(int n);
void mmb_str_free(char *p);
char *mmb_str_empty(void);
char *mmb_read_line(int hide);
char *mmb_str_set(char **slot, const char *s, int len, int maxlen, const char *what);
char *mmb_str_append(char **slot, const char *s, int len, int maxlen, const char *what);
void mmb_val_own(mmb_val *v, char **slot, int maxlen, const char *what);
double mmb_as_float(mmb_val v);
int64_t mmb_as_int(mmb_val v);
void mmb_need_num(mmb_val v);
mmb_val mmb_json_query(const char *js, const char *path);
void mmb_cmd_json_parse(void);
mmb_val mmb_json_stringify(mmb_val v);
void mmb_print_val(mmb_val v);
void mmb_out(const char *s);
void mmb_out_flush(void);
int mmb_print_font_w(void);
int mmb_print_font_h(void);
void mmb_print_cursor_goto(int px, int py);
void mmb_print_locate_pending(void);
int mmb_print_try_at(void);
void mmb_print_track(const char *s, unsigned n);
void mmb_cmd_locate(void);
void mmb_outf(const char *fmt_num, int64_t n); /* simple integer out */
void mmb_prof_reset(void);
void mmb_prof_report(void);
void mmb_tokenize_program(void);
const char *mmb_tok_line(int pc);
const char *mmb_tok_immediate(const char *src);
int mmb_kw_id(const char *kw);
void mmb_tokenize_text(const char *src, char *dst, int dstsz);
int mmb_match_token(const char *kw);
int mmb_tok_expand(char *dst, int dstsz);
void mmb_clear_vars(int keep_options);
void mmb_local_restore(int g);
void mmb_vars_rehash(void);
mmb_var *mmb_find_var(const char *name, int type, int create, int nidx, int *idx);
int mmb_var_offset(mmb_var *v, int nidx, const int *idx);
int mmb_elem_off(mmb_var *v, int nidx, const int *idx);
int mmb_parse_var_ref(char *name, int *nidx, int *idx);
mmb_val mmb_load_var(mmb_var *v, int off);
mmb_var *mmb_lookup_struct_var(const char *name);
void mmb_bind_struct_var(const char *name, int sid);
void mmb_tcache_invalidate(void);
int mmb_tcache_try_let(void);
int mmb_tcache_try_if(void);
void mmb_cmd_print(void);
void mmb_cmd_dim(void);
void mmb_cmd_redim(void);
void mmb_cmd_common(void);
void mmb_cmd_swap(void);
void mmb_struct_clear(void);
void mmb_struct_prepare(void);
int mmb_struct_lookup(const char *name);
mmb_sdef *mmb_struct_def(int idx);
int mmb_try_struct_fun(mmb_val *out);
void mmb_cmd_type(void);
void mmb_cmd_end_type(void);
void mmb_cmd_struct(void);
void mmb_cmd_list_type(void);
unsigned char *mmb_struct_elem(mmb_var *v, int off);
int mmb_try_parse_arrview(mmb_arrview *out);
double mmb_arrview_get(mmb_arrview a, int i);
void mmb_arrview_set(mmb_arrview a, int i, double x);
int mmb_arrview_int(mmb_arrview a, int i);
int mmb_struct_resolve(mmb_var *v, const char *path, int nidx, const int *idx);
int mmb_decode_part(const char *s, int len, char *name, int *nidx, int *idx);
void mmb_struct_store_member(mmb_var *v, int eoff, mmb_val val);
mmb_val mmb_struct_load_member(mmb_var *v, int eoff);
void mmb_cmd_local(void);
void mmb_cmd_static(void);
void mmb_cmd_error(void);
void mmb_cmd_memory(void);
void mmb_cmd_randomize(void);
void mmb_cmd_inc(void);
void mmb_cmd_dec(void);
void mmb_cmd_cat(void);
void mmb_cmd_on(void);
void mmb_cmd_continue(void);
void mmb_cmd_exit(void);
void mmb_cmd_mid(void);
void mmb_cmd_lset(void);
void mmb_cmd_rset(void);
void mmb_cmd_bit(void);
void mmb_cmd_byte(void);
void mmb_cmd_execute(void);
void mmb_cmd_array(void);
void mmb_cmd_sort(void);
void mmb_cmd_clear(void);
void mmb_cmd_new(void);
void mmb_cmd_list(void);
void mmb_cmd_run(void);
void mmb_cmd_end(void);
void mmb_cmd_chain(void);
void mmb_cmd_resume(void);
int mmb_parse_target(void);
void mmb_cmd_if(void);
void mmb_cmd_for(void);
void mmb_cmd_next(void);
void mmb_cmd_option(void);
void mmb_cmd_options(void);
void mmb_cmd_help(void);
void mmb_ihelp_open(const char *topic);
int mmb_in_ihelp(void);
const char *mmb_ihelp_key(char c);
void mmb_ihelp_poll(void);
int mmb_help_topic_count(void);
const char *mmb_help_topic_name(int i);
const char *mmb_help_topic_text(int i);
int mmb_help_topic_kind(int i);
int mmb_help_lookup(const char *name);
int mmb_help_alias_count(void);
const char *mmb_help_alias_name(int i);
const char *mmb_help_alias_canon(int i);
void mmb_cmd_math(void);
int mmb_try_math_fn(mmb_val *out);
void mmb_cmd_graphics(const char *kw);
void mmb_cmd_files(const char *kw);
void mmb_cmd_files_ui(void);
int mmb_in_files(void);
int mmb_files_take_prompt(void);
const char *mmb_files_key(char c);
void mmb_files_poll(void);
const char *mmb_files_resume(void);
const char *mmb_files_on_editor_exit(void);
void mmb_files_close(void);
void mmb_package_wiz_open(void);
int mmb_in_package(void);
const char *mmb_package_key(char c);
void mmb_package_poll(void);
void mmb_cmd_play(void);
void mmb_cmd_beep(void);
void mmb_cmd_juke(void);
int mmb_in_juke(void);
const char *mmb_juke_key(char c);
void mmb_juke_poll(void);
void mmb_cmd_load(void);
void mmb_cmd_edit(void);
void mmb_cmd_open(void);
void mmb_cmd_close(void);
int mmb_file_is_tcp(int fn);
int mmb_tcp_any_open(void);
void mmb_file_close_n(int fn);
void mmb_close_tcp_files(void);
int mmb_file_read(int fn, char *buf, int nch);
void mmb_file_write(int fn, const char *buf, unsigned n);
void mmb_cmd_chdir(void);
void mmb_cmd_mkdir(void);
void mmb_cmd_rmdir(void);
void mmb_cmd_kill(void);
void mmb_cmd_copy(void);
void mmb_cmd_xfer(void);
void mmb_cmd_name(void);
void mmb_cmd_cat_file(const char *arg);
void mmb_cmd_pause(void);
void mmb_cmd_vsync_wait(void);
void mmb_cmd_cls(void);
void mmb_cmd_mode(void);
void mmb_cmd_colour(void);
void mmb_cmd_page(void);
void mmb_cmd_text(void);
void mmb_cmd_font(void);
void mmb_cmd_blit(void);
void mmb_cmd_rbox(void);
void mmb_cmd_arc(void);
void mmb_cmd_triangle(void);
void mmb_cmd_polygon(void);
void mmb_cmd_pixel(void);
void mmb_cmd_line(void);
void mmb_cmd_box(void);
void mmb_cmd_circle(void);
void mmb_cmd_image(void);
void mmb_cmd_framebuffer(void);
void mmb_cmd_turtle(void);
void mmb_cmd_bitmap(void);
void mmb_cmd_screenshot(void);
int mmb_screenshot_capture(const char *path);
void mmb_screenshot_default_path(char *out, int outsz);
int mmb_screenshot_hotkey(char *msg, int msgsz);
void mmb_front_poll(void);
void mmb_cmd_save(void);
void mmb_cmd_input(void);
void mmb_cmd_line_input(void);
void mmb_cmd_seek(void);
void mmb_cmd_goto(void);
void mmb_cmd_gosub(void);
void mmb_cmd_return(void);
void mmb_cmd_while(void);
void mmb_cmd_wend(void);
void mmb_cmd_do(void);
void mmb_cmd_loop(void);
void mmb_cmd_exit_do(void);
void mmb_cmd_select(void);
void mmb_cmd_case(void);
void mmb_cmd_end_select(void);
void mmb_cmd_endif(void);
void mmb_cmd_data(void);
void mmb_cmd_read(void);
void mmb_cmd_restore(void);
void mmb_cmd_const(void);
void mmb_cmd_call(void);
void mmb_cmd_sub(void);
void mmb_cmd_function(void);
void mmb_cmd_end_sub(void);
void mmb_cmd_end_function(void);

void mmb_gfx_init(void);
void mmb_gfx_apply_default_mode(void);
void mmb_gfx_reapply_mode(void);
void mmb_gfx_set_mode(int mode, int bits);
int mmb_gfx_mode_for_size(int w, int h);
void mmb_gfx_reset_console(int wipe);
void mmb_gfx_cls(unsigned rgb);
void mmb_gfx_plot(int x, int y, unsigned rgb);
unsigned mmb_gfx_get(int x, int y);
unsigned mmb_gfx_get_page(int x, int y, int page);
void mmb_gfx_line(int x0, int y0, int x1, int y1, unsigned rgb, int lw);
void mmb_gfx_box(int x, int y, int w, int h, unsigned rgb, int lw, int fill);
void mmb_gfx_circle(int cx, int cy, int r, unsigned rgb, int lw, int fill);
void mmb_gfx_rbox(int x, int y, int w, int h, int r, unsigned rgb, int lw, int fill);
void mmb_gfx_triangle(int x1, int y1, int x2, int y2, int x3, int y3, unsigned rgb, int fill);
void mmb_gfx_fill_polygon(const int *xs, const int *ys, int n, unsigned rgb);
void mmb_gfx_text(int x, int y, const char *s, unsigned rgb);
extern const unsigned char mmb_cp437_8x16[256 * 16];
extern const unsigned char mmb_tnr_8x16[256 * 16];
extern const unsigned char mmb_tnr_16x32[95 * 32 * 2];
extern const unsigned char mmb_tnr_24x48[95 * 48 * 3];
extern const unsigned char mmb_tnr_32x64[95 * 64 * 4];
void mmb_gfx_glyph_cp437(int x, int y, unsigned ch, unsigned rgb);
void mmb_gfx_glyph_cell(int x, int y, unsigned ch, unsigned fg, unsigned bg);
void mmb_gfx_fill_rect(int x, int y, int w, int h, unsigned rgb);
void mmb_gfx_copy_rect(int srcpage, int dstpage, int x, int y, int w, int h);
void mmb_gfx_clear_overlay(void);
void mmb_gfx_present(void);
void mmb_gfx_present_rect(int x, int y, int w, int h);
void mmb_gfx_present_if(int page);
void mmb_gfx_dirty_reset(void);
void mmb_gfx_dirty_add(int x, int y, int w, int h);
void mmb_gfx_dirty_flush(void);
uint16_t *mmb_gfx_buf_for(int page, int *w, int *h);
unsigned mmb_rgb_to_native(unsigned rgb888);
unsigned mmb_native_to_rgb(unsigned native);
unsigned mmb_pix_load(uint16_t pix, unsigned alpha_byte);
uint16_t mmb_pix_store(unsigned rgb888, unsigned *alpha_out);
int mmb_gfx_map_y(int y, int h);
int mmb_gfx_writing_fb(void);
void ensure_page1_alpha(void);
void mmb_gfx_fb_create(int w, int h);
void mmb_gfx_fb_write(void);
void mmb_gfx_fb_backup(void);
void mmb_gfx_fb_restore(int x, int y, int w, int h, int all);
void mmb_gfx_fb_window(int x, int y, int page);
void mmb_gfx_fb_close(void);
void mmb_blit_copy_u16(uint16_t *dst, const uint16_t *src, unsigned n);
void mmb_blit_copy_rect16(uint16_t *dst, int dst_stride,
			 const uint16_t *src, int src_stride,
			 int w, int h);
void mmb_blit_sprite_row16(uint16_t *dst, const uint16_t *src, unsigned n);
void mmb_blit_sprite_trans16(uint16_t *dst, int dst_stride,
			     const uint16_t *src, int src_stride,
			     int w, int h);
void mmb_gfx_blit_copy(int x1, int y1, int x2, int y2, int w, int h, int srcpage, int ori);
void mmb_gfx_blit_read(int n, int x, int y, int w, int h, int srcpage);
void mmb_gfx_blit_write(int n, int x, int y, int ori);
void mmb_gfx_blit_close(int n);
void mmb_gfx_image_resize(int x, int y, int w, int h, int nx, int ny, int nw, int nh,
			  int srcpage, int fast, int skip_black);
void mmb_gfx_image_rotate(int x, int y, int w, int h, int nx, int ny, double angle,
			  int srcpage, int fast, int skip_black);
void mmb_gfx_image_warp_h(int x, int y, int w, int h, int x1, int y1, int h1,
			  int x2, int y2, int h2, int srcpage, int skip_black);
void mmb_gfx_image_warp_v(int x, int y, int w, int h, int x1, int y1, int w1,
			  int x2, int y2, int w2, int srcpage, int skip_black);
void mmb_gfx_page_scroll(int page, int dx, int dy, int fill, int has_fill);
void mmb_gfx_page_logic(int op, int p1, int p2, int dst);
void mmb_gfx_box_logic(int op, int x, int y, int w, int h, unsigned col, int page);
void mmb_gfx_bitmap(int x, int y, const unsigned char *bits, int nbytes,
		    int bw, int bh, int scale, unsigned fg, unsigned bg, int fill_bg);
void mmb_gfx_fill_poly(const int *xs, const int *ys, int n, unsigned rgb);
void mmb_turtle_init_state(int cls);
unsigned mmb_rgb_pack(int r, int g, int b);
unsigned mmb_rgb_pack_a(int r, int g, int b, int a);
void mmb_rgb_unpack(unsigned c, int *r, int *g, int *b);
unsigned mmb_quantize(unsigned rgb888);
unsigned mmb_named_colour(const char *name, int *ok);
unsigned mmb_ibm_colour(int n);
unsigned mmb_colour_from_int(int64_t v);

void mmb_vfs_init(void);
int mmb_vfs_chdir(const char *path);
int mmb_vfs_mkdir(const char *path);
int mmb_vfs_rmdir(const char *path);
int mmb_vfs_kill(const char *path);
int mmb_vfs_copy(const char *src, const char *dst);
int mmb_vfs_rename(const char *src, const char *dst);
/* Newline listing. Fills `out` with matching names, folders first, sorted;
 * *truncated is set (when non-NULL) if the folder held more names than fit in
 * `outsz` so the caller can report the cut instead of silently dropping the
 * tail (#693). */
int mmb_vfs_list(const char *spec, char *out, int outsz, int *truncated);
/* Structured listing (#621): one pass yields name, type and size so callers
 * that draw a size column (FILES) do not stat every file again. Entries come
 * back folders-first, then case-insensitively by name. Returns the count, or
 * -1 on error; *truncated is set when the directory held more than `max`
 * entries. The mmb_dirent type lives in mmbasic.h (shared with the Circle
 * storage backend). */
int mmb_vfs_list_entries(const char *spec, mmb_dirent *out, int max,
			 int *truncated);
int mmb_glob_match(const char *name, const char *pat);
int mmb_vfs_write(const char *path, const void *data, unsigned n, int append);
/* Streaming writes: open the target once, append chunks, then close. Avoids a
 * realloc (A:) or open/seek/write/close cycle (FAT) per received chunk.
 * Returns a handle >= 0, or -1. Handles are closed on error/abort. */
int mmb_vfs_wopen(const char *path, int append);
int mmb_vfs_wwrite(int handle, const void *data, unsigned n);
int mmb_vfs_wclose(int handle);
int mmb_vfs_read(const char *path, void *data, unsigned maxn, unsigned *n);
int mmb_vfs_read_at(const char *path, unsigned pos, void *data, unsigned n, unsigned *got);
int mmb_vfs_exists(const char *path);
int mmb_vfs_size(const char *path);
int mmb_vfs_resolve(const char *path, char *out, int outsz);
void mmb_vfs_drives(char *out, int outsz);
const char *mmb_vfs_cwd(void);
/* Reset the active console's working directory to the ramdisk root. */
void mmb_vfs_cwd_reset(void);
void mmb_vfs_seed_file(const char *path, const void *data, unsigned n);int mmb_vfs_read_ptr(const char *path, const unsigned char **ptr, unsigned *n);
void mmb_cmd_drive(void);
void mmb_cmd_eject(void);

/* Physical volumes C: (SD) and D+ (USB). Implemented in console/storage.cpp. */
int mmb_fat_ready(int letter);
int mmb_fat_chdir(int letter, const char *path);
int mmb_fat_mkdir(int letter, const char *path);
int mmb_fat_rmdir(int letter, const char *path);
int mmb_fat_unlink(int letter, const char *path);
int mmb_fat_rename(int letter, const char *from, const char *to);
int mmb_fat_list(int letter, const char *dir, const char *pat, char *out,
		 int outsz, int *truncated);
/* Structured variant of mmb_fat_list (#621): fills `out` with up to `max`
 * entries in one directory scan, using the backend's own type/size metadata
 * (FatFs FILINFO / POSIX dirent + one stat per file) so callers do not repeat
 * the lookup. See mmb_dirent. */
int mmb_fat_list_entries(int letter, const char *dir, const char *pat,
			 mmb_dirent *out, int max, int *truncated);
int mmb_fat_write(int letter, const char *path, const void *data, unsigned n, int append);
/* Streaming FAT writes; handle is an opaque open file (see mmb_vfs_wopen). */
void *mmb_fat_wopen(int letter, const char *path, int append);
int mmb_fat_wwrite(void *handle, const void *data, unsigned n);
int mmb_fat_wclose(void *handle);
int mmb_fat_read_at(int letter, const char *path, unsigned pos, void *data, unsigned n, unsigned *got);
int mmb_fat_size(int letter, const char *path);
int mmb_fat_exists(int letter, const char *path);
int mmb_fat_isdir(int letter, const char *path);
void mmb_fat_drive_line(int letter, char *out, int outsz);
/* Friendly volume label (FAT name or host mount name); "" when none. */
int mmb_fat_label(int letter, char *out, int outsz);
/* Safe eject/unmount of a removable volume; 0 on success. */
int mmb_fat_eject(int letter);
void mmb_storage_poll(void);

void mmb_storage_unmount(void);

/* Hotplug/idle notices from the storage layer (USB mount/unmount, eject).
 * Printed at the prompt unless a full-screen app claims them (FILES hint). */
void mmb_storage_notice(const char *msg);
int mmb_files_notice(const char *msg);

int mmb_find_line_pc(int num);
int mmb_const_lookup(const char *name, int type, mmb_val *out);
void mmb_const_define(const char *name, int type, mmb_val val);
void mmb_clear_consts(void);

void mmb_option_reset(void);
void mmb_option_list(int all);
void mmb_settings_load(void);
void mmb_settings_save(void);
const char *mmb_settings_path(void);
void mmb_cmd_factory_reset(void);
int mmb_vfs_hidden_name(const char *name);

int mmb_vfs_isdir(const char *path);
int mmb_vfs_readonly_path(const char *path);

/* ZIP stores the entry count in the 16-bit end-of-central-directory field, so
 * that is the format ceiling. The writer's entry table is heap-allocated and
 * grows on demand (#712); the old fixed 64-entry array capped PACKAGE well
 * below what the format allows. */
#define MMB_ZIP_MAX_FILES 65535
/* Store-mode archive ceiling for PACKAGE / RUN name.app. 16 MiB leaves room
 * for program bundles with assets; packaging and mounting share this. */
#define MMB_ZIP_MAX_BYTES (16u * 1024u * 1024u)

typedef struct mmb_zip_ent {
	char name[128];
	unsigned local_off, size, crc, nlen;
} mmb_zip_ent;

typedef struct mmb_zip_w {
	unsigned char *buf;
	unsigned cap, len;
	mmb_zip_ent *ent; /* heap-allocated, grows with nent (#712) */
	int nent;
	int ent_cap;
} mmb_zip_w;

typedef int (*mmb_zip_file_fn)(const char *path, const void *data, unsigned n, void *ctx);

int mmb_zip_begin(mmb_zip_w *z);
int mmb_zip_add(mmb_zip_w *z, const char *name, const void *data, unsigned n);
int mmb_zip_finish(mmb_zip_w *z, unsigned char **out, unsigned *n);
void mmb_zip_abort(mmb_zip_w *z);
int mmb_zip_foreach(const unsigned char *zip, unsigned n, mmb_zip_file_fn fn, void *ctx);
int mmb_zip_path_ok(const char *name);
unsigned mmb_crc32(const void *data, unsigned n);
int mmb_inflate(const unsigned char *in, unsigned in_len,
		unsigned char *out, unsigned out_cap, unsigned *out_len);

int mmb_pkg_mounted(void);
void mmb_pkg_unmount(void);
int mmb_pkg_mount(const char *path);
int mmb_pkg_is_name(const char *path);
void mmb_cmd_package(void);
void mmb_cmd_unpack(void);

/* App PATH, bare-token runner, Ctrl+Space picker, and boot launcher
 * (#515 boot-to-app / home, #520 PATH + picker). */
int mmb_app_resolve(const char *name, char *out, int outsz);
int mmb_apptui_active(void);
int mmb_apptui_launcher(void);
const char *mmb_apptui_key(char c);
void mmb_apptui_open(int launcher);
void mmb_apptui_poll(void);
void mmb_cmd_apps(void);
void mmb_boot_start(void);

/* System-wide appearance settings: the SETTINGS theme picker (#509). */
void mmb_cmd_settings(void);
int mmb_settings_active(void);
const char *mmb_settings_key(char c);
void mmb_settings_open(void);
void mmb_settings_poll(void);

int mmb_wlan_available(void);
int mmb_wlan_radio_pending(void);
int mmb_wlan_scan(char ssids[][64], int maxn);
int mmb_wlan_start(const char *ssid, const char *psk);
int mmb_wlan_connect(const char *ssid, const char *psk);
int mmb_wlan_status(void);
int mmb_wlan_ip(char *buf, int bufsize);
int mmb_wlan_ipconfig(char *buf, int bufsize);
void mmb_wlan_poll(void);
void mmb_wlan_apply_country(void);
int mmb_wifi_country_normalize(const char *s, char out[3]);

int mmb_eth_available(void);
int mmb_eth_start(void);
int mmb_eth_status(void);
int mmb_eth_wait_dhcp(unsigned ms);
int mmb_eth_ip(char *buf, int bufsize);
int mmb_eth_ipconfig(char *buf, int bufsize);

int mmb_net_available(void);
int mmb_net_gateway_ok(int force);
int mmb_net_tcp_open(const char *host, int port);
int mmb_net_tcp_begin(const char *host, int port);
int mmb_net_tcp_status(void);
int mmb_net_tcp_cancelling(void);
const char *mmb_net_tcp_errmsg(void);
const char *mmb_net_tcp_close_reason(void);
int mmb_net_tcp_send(const void *data, unsigned n);
int mmb_net_tcp_recv(void *data, unsigned maxn);
int mmb_net_tcp_rx_avail(void);
int mmb_net_tcp_peer_closed(void);
void mmb_net_tcp_close(void);
void mmb_net_tcp_debug_poll(void);
void mmb_net_yield(void);

/* One UDP request/response round trip (NTP). Sends tx to host:port and waits
 * up to timeout_ms for a datagram. Returns bytes received, 0 on timeout,
 * <0 on DNS/socket error. */
int mmb_net_udp_roundtrip(const char *host, int port,
			  const void *tx, unsigned txlen,
			  void *rx, unsigned rxcap, int timeout_ms);

int mmb_net_srv_listen(int port);
int mmb_net_srv_port(int lsn);
void mmb_net_srv_listen_close(int lsn);
int mmb_net_srv_accept(int lsn);
int mmb_net_srv_recv(int conn, void *data, unsigned maxn);
int mmb_net_srv_eof(int err);
const char *mmb_net_srv_reason(int err);
int mmb_net_srv_send(int conn, const void *data, unsigned n);
int mmb_net_srv_closed(int conn);
void mmb_net_srv_close(int conn);
int mmb_net_srv_ip(char *buf, int bufsize);

int mmb_ftp_start(const char *root, int port);
void mmb_ftp_stop(void);
void mmb_ftp_poll(void);
int mmb_ftp_running(void);
int mmb_ftp_port(void);
const char *mmb_ftp_status(void);

void mmb_cmd_connect(void);
void mmb_cmd_term(void);
void mmb_cmd_ipconfig(void);
void mmb_cmd_reboot(void);
void mmb_cmd_quit(void);
void mmb_check_break(void);
int mmb_in_connect(void);
const char *mmb_connect_key(char c);
void mmb_connect_poll(void);

int mmb_in_term(void);
const char *mmb_term_key(char c);
void mmb_term_poll(void);
void mmb_term_log_enable(int on);
void mmb_term_scrollback_set(int lines);

void mmb_cmd_wordpad(void);
int mmb_in_wordpad(void);
const char *mmb_wordpad_key(char c);
void mmb_wordpad_poll(void);
void mmb_cmd_paint(void);
int mmb_in_paint(void);
const char *mmb_paint_key(char c);
void mmb_paint_poll(void);
int mmb_mouse_read(mmb_mouse_state *out);
void mmb_cmd_credits(void);
void mmb_cmd_afk(void);
int mmb_in_afk(void);
void mmb_afk_key(char c);
void mmb_afk_poll(void);

void mmb_editor_open(const char *path);
const char *mmb_editor_feed(char c);
void mmb_editor_poll(void);
int mmb_editor_theme_count(void);
const char *mmb_editor_theme_name(int i);
int mmb_editor_theme_lookup(const char *s);

typedef struct mmb_ed_theme {
	const char *name;
	unsigned char menu_fg, menu_bg, hot;
	unsigned char sel_fg, sel_bg;
	unsigned char edit_fg, edit_bg;
	unsigned char mark_fg, mark_bg;
	unsigned char str_fg, num_fg, cmt_fg;
	unsigned char brd_fg, brd_bg;
	unsigned char tab_fg, tab_bg, tabcur_fg, tabcur_bg;
	unsigned char dlg_fg, dlg_bg;
	unsigned char sh_fg, sh_bg;
	unsigned char list_bg;
	unsigned char err_fg, err_bg;
	unsigned char field_fg, field_bg;
	const unsigned *pal;
} mmb_ed_theme;

const mmb_ed_theme *mmb_editor_theme(void);
const unsigned *mmb_editor_palette(void);
void mmb_editor_apply_tui_palette(void);
int mmb_editor_theme_field(const char *name, unsigned char *idx);
int mmb_editor_theme_rgb(const char *name, unsigned *rgb);

typedef struct mmb_ramdisk_entry {
	const char *path;
	const unsigned char *data;
	unsigned len;
} mmb_ramdisk_entry;

void mmb_ramdisk_seed(void);

void mmb_play_stop(void);
void mmb_play_mix(void);
void mmb_play_pause(int on);
int mmb_play_take_ended(void);
/* Read-only analyser feed for the JUKE visualiser. */
#define MMB_AUDIO_BANDS 24
#define MMB_AUDIO_SCOPE 256
void mmb_audio_spectrum(float *bands, int nbands);
int mmb_audio_scope(short *out, int n);
void mmb_audio_apply_options(void);
int mmb_play_mp3(const char *path);
int mmb_play_mod(const char *path);
int mmb_play_xm(const char *path);
int mmb_load_jpeg(const char *path, int x, int y);
int mmb_load_png(const char *path, int x, int y, int has_trans, unsigned trans_rgb);
int mmb_img_probe(const char *path, int *w, int *h);
int mmb_png_decode_rgba(const unsigned char *file, unsigned n,
			uint32_t **out, int *w, int *h);
int mmb_png_encode_rgb(const unsigned char *rgb, int w, int h,
		       unsigned char **out, unsigned *out_len);
/* ---- PCX codec (#631): encode/decode for PAINT and the FILES preview ---- */
int mmb_pcx_decode_rgba(const unsigned char *file, unsigned n,
			uint32_t **out, int *w, int *h);
int mmb_pcx_decode_indexed(const unsigned char *file, unsigned n,
			   unsigned char **idx, int *w, int *h,
			   unsigned char *pal, int *ncol);
int mmb_pcx_encode_indexed(const unsigned char *idx, int w, int h,
			   const unsigned char *pal,
			   unsigned char **out, unsigned *out_len);
void mmb_clock_init(void);
void mmb_clock_refresh(void);
int mmb_clock_set_date(const char *s);
int mmb_clock_set_time(const char *s);
int mmb_clock_set_epoch(int64_t utc_epoch);
int mmb_tz_offset_min(void);
int mmb_timezone_normalize(const char *s, int *offset_min, char *out, int outcap);
int64_t mmb_epoch_make(int y, int mo, int d, int h, int mi, int s);
void mmb_epoch_break(int64_t e, int *py, int *pmo, int *pd, int *ph, int *pmi, int *ps);
int64_t mmb_epoch_now(void);

/* ---- NTP clock sync (#524) -------------------------------------------- */
int mmb_ntp_parse_server(const char *spec, char *host, int hostcap, int *port);
int mmb_ntp_sync(const char *server, int64_t *out_epoch);
const char *mmb_ntp_errmsg(int rc);
void mmb_ntp_poll(void);
void mmb_cmd_ntp(void);
/* ---- Host OS clipboard bridge (#525) ---------------------------------- */
/* 1 when the active platform exposes a host text clipboard (native SDL
 * builds); 0 on the bare-metal Pi, where the bridge is a no-op. */
int mmb_clipboard_available(void);
/* Host clipboard text as a malloc'd UTF-8 string (caller frees), or NULL. */
char *mmb_clipboard_get(void);
/* Store UTF-8 text on the host clipboard. Returns 0 on success, -1 when the
 * platform has no clipboard (Pi). */
int mmb_clipboard_set(const char *utf8);
int mmb_clipboard_setn(const char *s, unsigned n);

int mmb_inkey_pop(void);
int mmb_keydown_get(int n);
void mmb_run_events(void);
void mmb_cmd_settick(void);
void mmb_cmd_sprite(void);
void mmb_sprite_reset(void);
int mmb_call_named_sub(const char *name);
int mmb_try_user_function(mmb_val *out);
int mmb_play_wav(const char *path);
void mmb_play_tts(void);


int mmb_keyword_eq(const char *a, const char *b);
void mmb_upper(char *s);
unsigned mmb_now_ms(void);

/* functions inside expressions */
int mmb_try_function(mmb_val *out);

#endif
