#ifndef MMB_PRIV_H
#define MMB_PRIV_H

#include "mmbasic.h"
#include <circle/setjmp.h>
#include <circle/alloc.h>
#include <circle/util.h>
#include <stdint.h>
#include <stddef.h>

#define MMB_MAX_NAME      40
#define MMB_MAX_STR       255
#define MMB_MAX_VARS      256
#define MMB_MAX_DIMS      4
#define MMB_MAX_LINES     512
#define MMB_LINE_LEN      256
#define MMB_MAX_FILES     10
#define MMB_MAX_PAGES     8
#define MMB_OUT_LEN       4096
#define MMB_PROG_NAME     80
#define MMB_MAX_GOSUB     16
#define MMB_MAX_CTRL      32
#define MMB_MAX_CONST     64
#define MMB_MAX_SUBS      64

#define T_NUM   1
#define T_INT   2
#define T_STR   4

typedef struct mmb_val {
	int type;          /* T_NUM, T_INT, T_STR */
	double f;
	int64_t i;
	char s[MMB_MAX_STR + 1];
} mmb_val;

typedef struct mmb_var {
	char name[MMB_MAX_NAME];
	int type;
	int dims;
	int dim[MMB_MAX_DIMS]; /* inclusive upper bound */
	int size;              /* element count */
	union {
		double *f;
		int64_t *i;
		char **s;
	} data;
	int used;
} mmb_var;

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
	int escape;
	char search_path[128];
	char fkey[12][65];     /* F1..F12 */
	int list_changed_only;
	int error_continue;    /* 0 ABORT (default) 1 CONTINUE */
} mmb_options;

typedef struct mmb_file {
	int open;
	int mode; /* 0 input 1 output 2 append */
	int pos;
	char path[128];
} mmb_file;

typedef struct mmb_gfx {
	int mode;
	int bits;
	int w, h;
	int pages;
	int write_page;
	int display_page;
	unsigned fg, bg;
	int font, font_scale;
	uint32_t *page[MMB_MAX_PAGES]; /* RGB888 */
} mmb_gfx;

typedef struct mmb_editor {
	int active;
	char path[128];
	char buf[16384];
	int len;
	int cx, cy, row0;
	int dirty;
	int run_on_exit;
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
	char prog[MMB_MAX_LINES][MMB_LINE_LEN];
	int prog_num[MMB_MAX_LINES];
	int nprog;
	char current_prog[MMB_PROG_NAME];
	int running;
	int for_sp;
	struct {
		char var[MMB_MAX_NAME];
		int64_t to, step;
		int line, stmt;
	} forstack[16];
	mmb_gfx gfx;
	mmb_file files[MMB_MAX_FILES + 1];
	mmb_editor ed;
	mmb_audio audio;
	char cwd[128];
	int drive;             /* 'A'..'H', default 'A' (ramdisk) */
	int data_line, data_pos;
	int gosub_sp;
	int gosub_stack[MMB_MAX_GOSUB];
	int branch_pc;         /* GOTO/GOSUB/RETURN/loop control */
	int run_pc;            /* current program line index */
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
	int nconst;
	struct {
		char name[MMB_MAX_NAME];
		int type;
		mmb_val val;
		int used;
	} consts[MMB_MAX_CONST];
	int nsubs;
	struct {
		char name[MMB_MAX_NAME];
		int line_pc;
		int is_func;
		int used;
	} subs[MMB_MAX_SUBS];
	int in_sub;            /* executing inside sub body */
	int home_prompt;       /* CLS: next immediate prompt has no leading CR/LF */
} mmb;

extern mmb G;

void mmb_error(const char *msg);
void mmb_syntax(void);
void mmb_skip_sp(void);
int mmb_match(const char *kw);
int mmb_match_exact(const char *kw);
void mmb_expect(char c);
int mmb_is_ident(char c);
int mmb_is_digit(char c);
void mmb_ident(char *dst, int dstsz);
int mmb_type_suffix(char *name); /* strips $ % ! and returns type, 0 if none */
mmb_val mmb_expr(void);
mmb_val mmb_num_val(double f);
mmb_val mmb_int_val(int64_t i);
mmb_val mmb_str_val(const char *s);
double mmb_as_float(mmb_val v);
int64_t mmb_as_int(mmb_val v);
void mmb_need_num(mmb_val v);
void mmb_print_val(mmb_val v);
void mmb_out(const char *s);
void mmb_outf(const char *fmt_num, int64_t n); /* simple integer out */
void mmb_clear_vars(int keep_options);
mmb_var *mmb_find_var(const char *name, int type, int create, int nidx, int *idx);
void mmb_cmd_print(void);
void mmb_cmd_dim(void);
void mmb_cmd_clear(void);
void mmb_cmd_new(void);
void mmb_cmd_list(void);
void mmb_cmd_run(void);
void mmb_cmd_end(void);
void mmb_cmd_if(void);
void mmb_cmd_for(void);
void mmb_cmd_next(void);
void mmb_cmd_option(void);
void mmb_cmd_help(void);
void mmb_cmd_graphics(const char *kw);
void mmb_cmd_files(const char *kw);
void mmb_cmd_play(void);
void mmb_cmd_load(void);
void mmb_cmd_edit(void);
void mmb_cmd_open(void);
void mmb_cmd_close(void);
void mmb_cmd_chdir(void);
void mmb_cmd_mkdir(void);
void mmb_cmd_rmdir(void);
void mmb_cmd_kill(void);
void mmb_cmd_copy(void);
void mmb_cmd_name(void);
void mmb_cmd_pause(void);
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
void mmb_gfx_set_mode(int mode, int bits);
void mmb_gfx_cls(unsigned rgb);
void mmb_gfx_plot(int x, int y, unsigned rgb);
unsigned mmb_gfx_get(int x, int y);
void mmb_gfx_line(int x0, int y0, int x1, int y1, unsigned rgb, int lw);
void mmb_gfx_box(int x, int y, int w, int h, unsigned rgb, int lw, int fill);
void mmb_gfx_circle(int cx, int cy, int r, unsigned rgb, int lw, int fill);
void mmb_gfx_rbox(int x, int y, int w, int h, int r, unsigned rgb, int lw, int fill);
void mmb_gfx_triangle(int x1, int y1, int x2, int y2, int x3, int y3, unsigned rgb, int fill);
void mmb_gfx_text(int x, int y, const char *s, unsigned rgb);
unsigned mmb_rgb_pack(int r, int g, int b);
void mmb_rgb_unpack(unsigned c, int *r, int *g, int *b);
unsigned mmb_quantize(unsigned rgb888);
unsigned mmb_named_colour(const char *name, int *ok);

void mmb_vfs_init(void);
int mmb_vfs_chdir(const char *path);
int mmb_vfs_mkdir(const char *path);
int mmb_vfs_rmdir(const char *path);
int mmb_vfs_kill(const char *path);
int mmb_vfs_copy(const char *src, const char *dst);
int mmb_vfs_rename(const char *src, const char *dst);
int mmb_vfs_list(const char *spec, char *out, int outsz);
int mmb_vfs_write(const char *path, const void *data, unsigned n, int append);
int mmb_vfs_read(const char *path, void *data, unsigned maxn, unsigned *n);
int mmb_vfs_read_at(const char *path, unsigned pos, void *data, unsigned n, unsigned *got);
int mmb_vfs_exists(const char *path);
int mmb_vfs_size(const char *path);
int mmb_vfs_resolve(const char *path, char *out, int outsz);
void mmb_vfs_drives(char *out, int outsz);
const char *mmb_vfs_cwd(void);
void mmb_vfs_seed_file(const char *path, const void *data, unsigned n);
int mmb_vfs_read_ptr(const char *path, const unsigned char **ptr, unsigned *n);
void mmb_cmd_drive(void);

/* Physical volumes C: (SD) and D+ (USB). Implemented in console/storage.cpp. */
int mmb_fat_ready(int letter);
int mmb_fat_chdir(int letter, const char *path);
int mmb_fat_mkdir(int letter, const char *path);
int mmb_fat_rmdir(int letter, const char *path);
int mmb_fat_unlink(int letter, const char *path);
int mmb_fat_rename(int letter, const char *from, const char *to);
int mmb_fat_list(int letter, const char *dir, const char *pat, char *out, int outsz);
int mmb_fat_write(int letter, const char *path, const void *data, unsigned n, int append);
int mmb_fat_read_at(int letter, const char *path, unsigned pos, void *data, unsigned n, unsigned *got);
int mmb_fat_size(int letter, const char *path);
int mmb_fat_exists(int letter, const char *path);
const char *mmb_fat_cwd(int letter);
void mmb_fat_drive_line(int letter, char *out, int outsz);
void mmb_storage_poll(void);

int mmb_find_line_pc(int num);
int mmb_const_lookup(const char *name, int type, mmb_val *out);
void mmb_const_define(const char *name, int type, mmb_val val);
void mmb_clear_consts(void);

void mmb_option_reset(void);
void mmb_option_list(int all);

void mmb_editor_open(const char *path);
const char *mmb_editor_feed(char c);

void mmb_play_stop(void);
int mmb_play_mp3(const char *path);
int mmb_play_mod(const char *path);
int mmb_play_xm(const char *path);
int mmb_load_jpeg(const char *path, int x, int y);
int mmb_load_png(const char *path, int x, int y);

void mmb_assets_seed(void);

int mmb_keyword_eq(const char *a, const char *b);
void mmb_upper(char *s);
unsigned mmb_now_ms(void);

/* functions inside expressions */
int mmb_try_function(mmb_val *out);

#endif
