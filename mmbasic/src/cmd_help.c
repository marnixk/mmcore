#include "mmb_priv.h"

/* Immediate-mode HELP. Text is static so it lives in the binary and is
 * emitted through mmb_out() the same way PRINT does. Only commands and
 * language constructs that this interpreter actually implements are
 * documented — no CMM2-only features.
 */

#define HELP_CMD  1
#define HELP_LANG 2

typedef struct {
	const char *name;
	int kind;
	const char *text;
} help_topic;

static const char kIndexCommands[] =
	"Commands  (HELP topic for details)\n"
	"\n"
	"Graphics\n"
	"  CLS PIXEL LINE BOX CIRCLE RBOX ARC TRIANGLE\n"
	"  POLYGON TEXT FONT COLOUR MODE PAGE BLIT\n"
	"\n"
	"Files\n"
	"  DIR FILES OPEN CLOSE CHDIR MKDIR RMDIR COPY RENAME\n"
	"  KILL DRIVE LOAD SAVE SEEK\n"
	"\n"
	"Program\n"
	"  NEW LIST RUN EDIT\n"
	"\n"
	"Other\n"
	"  PRINT INPUT OPTION PLAY PAUSE CLEAR END CALL HELP\n"
	"\n"
	"Type HELP BASIC for language, HELP FUNCTIONS for functions.";

static const char kIndexBasic[] =
	"Language constructs  (HELP BASIC topic or HELP topic)\n"
	"\n"
	"  FOR NEXT            WHILE WEND\n"
	"  DO LOOP EXIT DO     IF THEN ELSE ELSEIF ENDIF\n"
	"  SELECT CASE         DIM CONST\n"
	"  DATA READ RESTORE   SUB FUNCTION CALL\n"
	"  GOTO GOSUB RETURN   LET REM END\n"
	"\n"
	"Type HELP topic for syntax and examples.";

static const char kHelpHelp[] =
	"HELP\n"
	"HELP command\n"
	"HELP BASIC\n"
	"HELP BASIC construct\n"
	"\n"
	"Show built-in help. Keywords are case-insensitive\n"
	"(help, HELP, Help CLS all work).\n"
	"\n"
	"HELP lists commands that have help text.\n"
	"HELP command shows syntax and details.\n"
	"HELP BASIC lists language constructs.\n"
	"HELP BASIC construct (or HELP construct)\n"
	"shows syntax, notes, and short examples.\n"
	"\n"
	"Unknown topics print a message; they do not raise\n"
	"?SYNTAX ERROR.";

static const char kHelpCls[] =
	"CLS [colour]\n"
	"\n"
	"Clear the graphics screen and home the text cursor\n"
	"so the next prompt is at the top of HDMI.\n"
	"\n"
	"With no argument the screen is filled with the\n"
	"current background colour (see COLOUR).\n"
	"colour is an RGB integer: RGB(r,g,b) or RGB(\"RED\").\n"
	"\n"
	"Example:  CLS RGB(0,0,0)";

static const char kHelpPrint[] =
	"PRINT [expr] [;|, expr]...\n"
	"PRINT #fn, expr [;|, expr]...\n"
	"?  is an alias for PRINT.\n"
	"\n"
	"Write values to the console, or to an open file\n"
	"with PRINT #fn. PRINT always ends with a newline\n"
	"unless the statement ends with ; . A comma inserts\n"
	"a space between items. Bare PRINT prints a blank\n"
	"line.\n"
	"\n"
	"Example:  PRINT 6*7\n"
	"          PRINT #1, \"HELLO\"";

static const char kHelpPixel[] =
	"PIXEL x, y [, colour]\n"
	"PIXEL(x, y)   (function)\n"
	"\n"
	"Set the pixel at (x,y). colour defaults to the\n"
	"current foreground (see COLOUR).\n"
	"PIXEL(x,y) as a function returns that pixel's RGB.\n"
	"\n"
	"Example:  PIXEL 10,10,RGB(255,0,0)\n"
	"          PRINT PIXEL(10,10)";

static const char kHelpLine[] =
	"LINE x1, y1, x2, y2 [, colour] [, linewidth]\n"
	"\n"
	"Draw a straight line. colour defaults to the\n"
	"current foreground. linewidth defaults to 1.\n"
	"\n"
	"LINE INPUT #fn, var$ reads a file line (see that\n"
	"topic).\n"
	"\n"
	"Example:  LINE 0,0,100,80,RGB(0,255,0)";

static const char kHelpLineInput[] =
	"LINE INPUT #fn, var$\n"
	"\n"
	"Read one line from an open file into a string\n"
	"variable. var$ must be a string.\n"
	"\n"
	"Example:\n"
	"  OPEN \"N.TXT\" FOR INPUT AS #1\n"
	"  LINE INPUT #1, A$\n"
	"  CLOSE #1";

static const char kHelpBox[] =
	"BOX x, y, w, h [, lw] [, colour] [, fill]\n"
	"\n"
	"Draw a rectangle at (x,y) of size w by h.\n"
	"lw is line width (default 1). colour defaults to\n"
	"the foreground. fill >= 0 fills the interior\n"
	"(use an RGB value such as RGB(0,255,0)).\n"
	"\n"
	"Example:  BOX 20,20,40,40,1,RGB(0,255,0),1";

static const char kHelpCircle[] =
	"CIRCLE x, y, r [, lw] [, colour] [, fill]\n"
	"\n"
	"Draw a circle centred at (x,y) with radius r.\n"
	"Optional line width, colour, and fill as for BOX.\n"
	"\n"
	"Example:  CIRCLE 100,80,30,1,RGB(0,255,255)";

static const char kHelpRbox[] =
	"RBOX x, y, w, h [, radius] [, lw] [, fill] [, colour]\n"
	"\n"
	"Rounded rectangle. radius defaults to 8.\n"
	"If the last numeric argument is above 7 it is treated\n"
	"as an RGB colour.\n"
	"\n"
	"Example:  RBOX 80,20,40,40,6,RGB(0,0,255)";

static const char kHelpArc[] =
	"ARC x, y, r1, r2, a1, a2 [, colour]\n"
	"\n"
	"Elliptical arc centred at (x,y). r1 is the X radius,\n"
	"r2 the Y radius. a1 and a2 are start and end angles\n"
	"(radians, or degrees after OPTION ANGLE DEGREES).\n"
	"Drawn as line segments.\n"
	"\n"
	"Example:  OPTION ANGLE DEGREES\n"
	"          ARC 160,120,40,40,0,90,RGB(255,255,0)";

static const char kHelpTriangle[] =
	"TRIANGLE x1,y1, x2,y2, x3,y3 [, colour] [, fill]\n"
	"\n"
	"Draw a triangle. fill >= 0 fills the interior.\n"
	"\n"
	"Example:  TRIANGLE 10,80,40,80,25,50,RGB(255,255,0)";

static const char kHelpPolygon[] =
	"POLYGON x1,y1, x2,y2, x3,y3... [, colour] [, fill]\n"
	"\n"
	"Draw a closed polygon (at least 3 vertices).\n"
	"Optional colour and fill are the last arguments.\n"
	"Filled polygons are triangulated from the first vertex.\n"
	"\n"
	"Example:  POLYGON 10,10,40,10,25,30,RGB(255,0,0)";

static const char kHelpText[] =
	"TEXT x, y, string$ [, colour]\n"
	"\n"
	"Draw string$ at pixel (x,y) in the current font.\n"
	"colour defaults to the foreground.\n"
	"\n"
	"Example:  TEXT 8,8,\"HELLO\",RGB(255,255,255)";

static const char kHelpFont[] =
	"FONT n [, scale]\n"
	"\n"
	"Select font number n and optional integer scale\n"
	"(minimum 1). Used by TEXT.\n"
	"\n"
	"Example:  FONT 1,2";

static const char kHelpColour[] =
	"COLOUR fg [, bg]\n"
	"COLOR fg [, bg]     (alias)\n"
	"\n"
	"Set foreground and optional background colour.\n"
	"Colours are RGB integers: RGB(r,g,b) or RGB(\"RED\").\n"
	"Named colours include BLACK RED GREEN BLUE YELLOW\n"
	"CYAN MAGENTA WHITE ORANGE PINK GOLD BROWN GRAY.\n"
	"\n"
	"Example:  COLOUR RGB(255,0,0)";

static const char kHelpMode[] =
	"MODE n [, bits] [, clscolour]\n"
	"\n"
	"Set the graphics page size, colour depth, and HDMI\n"
	"framebuffer. n is 1 to 17. bits is 8, 12, 16 or 32\n"
	"(default 8 if omitted). A third argument then clears\n"
	"the page to that colour.\n"
	"\n"
	"MODE updates MM.HRES and MM.VRES, reallocates\n"
	"graphics pages, and retunes HDMI to the same width\n"
	"and height. PIXEL, LINE, BOX, TEXT and PAGE use that\n"
	"size. MM.INFO(MODE) is n + bits/100 (MODE 8,16 reads\n"
	"as 8.16).\n"
	"\n"
	"HDMI colour depth stays the compile-time screen\n"
	"depth (16-bit RGB565 here). bits still quantises the\n"
	"logical page (8=RGB332, 12=RGB444, 16=RGB565,\n"
	"32=RGB888). If the firmware cannot allocate a size,\n"
	"the logical page still changes and HDMI stays at the\n"
	"last working resolution.\n"
	"\n"
	"Pages: 8 when width*height is 800x600 or smaller,\n"
	"2 when larger.\n"
	"\n"
	"Depths (quantised; stored as 32-bit RGB):\n"
	"  8   RGB332\n"
	"  12  RGB444 (invalid on modes 9, 11, 12, 14)\n"
	"  16  RGB565\n"
	"  32  RGB888\n"
	"\n"
	"Boot default if HDMI is 640x480: MODE 8,16\n"
	"Otherwise: MODE 1,8\n"
	"Tests and examples reset with MODE 8,16.\n"
	"\n"
	"All supported modes (n, pixels, depths):\n"
	"  1   800x600      8,12,16,32\n"
	"  2   640x400      8,12,16,32\n"
	"  3   320x200      8,12,16,32\n"
	"  4   480x432      8,12,16,32\n"
	"  5   240x216      8,12,16,32\n"
	"  6   256x240      8,12,16,32\n"
	"  7   320x240      8,12,16,32\n"
	"  8   640x480      8,12,16,32\n"
	"  9   1024x768     8,16,32\n"
	" 10   848x480      8,12,16,32\n"
	" 11   1280x720     8,16,32\n"
	" 12   960x540      8,16,32\n"
	" 13   400x300      8,12,16,32\n"
	" 14   960x540      8,16,32\n"
	" 15   1280x1024    8,12,16,32\n"
	" 16   1920x1080    8,12,16,32\n"
	" 17   384x240      8,12,16,32\n"
	"\n"
	"Unknown n or bits raises ?INVALID MODE.\n"
	"\n"
	"Example:  MODE 8,16\n"
	"          PRINT MM.HRES, MM.VRES\n"
	"          MODE 17,8";

static const char kHelpPage[] =
	"PAGE WRITE n\n"
	"PAGE DISPLAY n\n"
	"PAGE COPY src [TO dst]\n"
	"\n"
	"Off-screen pages. WRITE selects the draw target.\n"
	"DISPLAY shows page n. COPY copies src to dst (or\n"
	"onto itself if TO is omitted).\n"
	"\n"
	"Example:  PAGE COPY 0 TO 1\n"
	"          PAGE WRITE 1";

static const char kHelpBlit[] =
	"BLIT sx, sy, w, h, dx, dy\n"
	"\n"
	"Copy a w by h rectangle from (sx,sy) to (dx,dy)\n"
	"on the current write page.\n"
	"\n"
	"Example:  BLIT 0,0,32,32,100,80";

static const char kHelpPlay[] =
	"PLAY STOP\n"
	"PLAY PAUSE\n"
	"PLAY RESUME\n"
	"PLAY VOLUME left [, right]\n"
	"PLAY TONE left, right [, duration]\n"
	"PLAY MP3 file$\n"
	"PLAY MODFILE file$     (PLAY MOD file$)\n"
	"PLAY XM file$          (PLAY XMFILE file$)\n"
	"\n"
	"Play audio from a file on the current drive, or a\n"
	"tone. PLAYING() is 1 while audio is playing and not\n"
	"paused.\n"
	"\n"
	"Example:  PLAY MP3 \"TEST.MP3\"\n"
	"          PRINT PLAYING()";

static const char kHelpEdit[] =
	"EDIT [file$]\n"
	"\n"
	"Open the colour TUI editor (menu bar, tabs, status).\n"
	"With no file, uses the current program name if set.\n"
	"\n"
	"Menus: Esc+F File  Esc+E Edit  Esc+R Run  Esc+H Help\n"
	"       F10 File menu   Esc+1..9 switch tabs\n"
	"File: Open, Save, Save As, Quit. Run saves and RUN.\n"
	"Keys: F2 save  F3 open  F9 run  ^O save  ^X quit\n"
	"      ^R save and run  ^K cut line  ^U paste\n"
	"\n"
	"Example:  EDIT \"HI.BAS\"";

static const char kHelpDir[] =
	"DIR [spec$]\n"
	"\n"
	"List files in the current directory, or matching\n"
	"optional spec$ (a path or wildcard).\n"
	"\n"
	"Example:  DIR\n"
	"          DIR \"A:/*.PNG\"";

static const char kHelpFiles[] =
	"FILES\n"
	"\n"
	"Open the dual-pane file manager (not a DIR listing).\n"
	"Navigate drives and folders. Enter on a directory\n"
	"opens it. Enter on a .BAS file RUNs it. View (v/F3)\n"
	"previews PNG/JPG or plays MP3/XM/MOD when supported.\n"
	"Tab switches panes. q or Esc leaves FILES.\n"
	"\n"
	"Example:  FILES";

static const char kHelpOpen[] =
	"OPEN file$ [FOR INPUT|OUTPUT|APPEND] AS #n\n"
	"\n"
	"Open a file. n is 1 to 10. OUTPUT creates/truncates.\n"
	"APPEND writes at the end. INPUT reads.\n"
	"\n"
	"Example:  OPEN \"A.TXT\" FOR OUTPUT AS #1\n"
	"          PRINT #1, \"HELLO\"\n"
	"          CLOSE #1";

static const char kHelpClose[] =
	"CLOSE #n\n"
	"\n"
	"Close file number n (1 to 10).\n"
	"\n"
	"Example:  CLOSE #1";

static const char kHelpLoad[] =
	"LOAD [JPG|JPEG|PNG] file$ [, x, y]\n"
	"\n"
	"Load an image onto the screen at (x,y) (default 0,0).\n"
	"JPG/JPEG uses the JPEG decoder. PNG (and BMP/GIF/\n"
	"IMAGE as a keyword) uses the PNG decoder. With no\n"
	"type keyword, .JPG/.JPEG selects JPEG; otherwise PNG.\n"
	"\n"
	"Example:  LOAD PNG \"TEST.PNG\"\n"
	"          LOAD JPG \"TEST.JPG\",0,0";

static const char kHelpSave[] =
	"SAVE file$\n"
	"\n"
	"Save the program in memory. .BAS is appended if\n"
	"file$ has no extension.\n"
	"\n"
	"Example:  SAVE \"P.BAS\"";

static const char kHelpRun[] =
	"RUN [file$]\n"
	"\n"
	"Run the program in memory. With file$, load that\n"
	"program first (.BAS appended if needed) then run.\n"
	"\n"
	"Example:  RUN\n"
	"          RUN \"HI.BAS\"";

static const char kHelpNew[] =
	"NEW\n"
	"\n"
	"Erase the program in memory and clear variables\n"
	"and constants. OPTION settings are kept.";

static const char kHelpList[] =
	"LIST\n"
	"\n"
	"List the program in memory with line numbers.";

static const char kHelpInput[] =
	"INPUT #n, var [, var...]\n"
	"INPUT var [, var...]\n"
	"LINE INPUT #fn, var$\n"
	"\n"
	"INPUT #n reads comma-separated values from an open\n"
	"file. Without #n, INPUT stores an empty value\n"
	"(typed console INPUT is not implemented).\n"
	"LINE INPUT reads a whole file line into a string.\n"
	"\n"
	"Example:  OPEN \"N.TXT\" FOR INPUT AS #1\n"
	"          INPUT #1, N\n"
	"          CLOSE #1";

static const char kHelpOption[] =
	"OPTION setting ...\n"
	"OPTION LIST [ALL]\n"
	"OPTION RESET\n"
	"\n"
	"Interpreter settings. Implemented subcommands:\n"
	"  BASE 0|1          EXPLICIT [ON|OFF]\n"
	"  DEFAULT INTEGER|FLOAT|STRING|NONE\n"
	"  DEFAULT MODE n    DEFAULT COLOURS fg [, bg]\n"
	"  ANGLE DEGREES|RADIANS    Y_AXIS UP|DOWN\n"
	"  TAB 2|3|4|8       BREAK n     AUTORUN ON|OFF\n"
	"  COLOURCODE ON|OFF|REVERSE\n"
	"  CONSOLE SCREEN|SERIAL|BOTH|NONE|SAVE|PORT n\n"
	"  CRLF CRLF|CR|LF   BAUDRATE n  CASE UPPER|LOWER|TITLE\n"
	"  LEGACY ON|OFF     MILLISECONDS ON|OFF\n"
	"  MOUSE OFF|n [, sens]   PIN n   PROFILING ON|OFF\n"
	"  RAM   FLASH [page]     STATUS ON|OFF\n"
	"  VCC n    SLEEP n    SD TIMING FAST|NORMAL\n"
	"  SERIAL PULLUP ENABLE|DISABLE\n"
	"  RTC CALIBRATE n   DS3231 ON|OFF   BASELINE ON|OFF\n"
	"  USBKEYBOARD US|UK|DE|FR|ES [, NOLED]\n"
	"  KEYBOARD REPEAT first [, next]\n"
	"  EDIT FONT SMALL|NORMAL|MEDIUM|LARGE|VERY LARGE\n"
	"  ESCAPE    SEARCH PATH path$    ERROR CONTINUE|ABORT\n"
	"  F1..F12 string$    AUDIO/DISPLAY/LCDPANEL/TOUCH/\n"
	"  WIFI/SDCARD/RESOLUTION/CLOCK/CPUSPEED/HEARTBEAT\n"
	"  (hardware lines are parsed and stored)\n"
	"\n"
	"Example:  OPTION BASE 1\n"
	"          OPTION LIST";

static const char kHelpChdir[] =
	"CHDIR path$\n"
	"\n"
	"Change the current directory. A drive letter such as\n"
	"\"C:\" or \"A:/DATA\" selects that drive.\n"
	"CWD$ returns the current path.\n"
	"\n"
	"Example:  CHDIR \"A:\"\n"
	"          PRINT CWD$";

static const char kHelpMkdir[] =
	"MKDIR path$\n"
	"\n"
	"Create a directory.\n"
	"\n"
	"Example:  MKDIR \"DATA\"";

static const char kHelpRmdir[] =
	"RMDIR path$\n"
	"\n"
	"Remove an empty directory.\n"
	"\n"
	"Example:  RMDIR \"DATA\"";

static const char kHelpCopy[] =
	"COPY src$ [TO] dst$\n"
	"COPY src$, dst$\n"
	"\n"
	"Copy a file.\n"
	"\n"
	"Example:  COPY \"A.TXT\" TO \"B.TXT\"";

static const char kHelpRename[] =
	"RENAME src$ [AS|TO] dst$\n"
	"NAME src$ AS dst$     (alias)\n"
	"\n"
	"Rename or move a file.\n"
	"\n"
	"Example:  RENAME \"B.TXT\" AS \"C.TXT\"";

static const char kHelpKill[] =
	"KILL path$\n"
	"\n"
	"Delete a file. There is no DELETE command.\n"
	"\n"
	"Example:  KILL \"Z.TXT\"";

static const char kHelpDrive[] =
	"DRIVE [letter$]\n"
	"\n"
	"With no argument, list available drives.\n"
	"With a letter such as \"A:\" or \"C:\", make that the\n"
	"current drive. A: is the RAM disk. C: is the SD card\n"
	"slot. D: and later are USB volumes when present.\n"
	"\n"
	"Example:  DRIVE\n"
	"          DRIVE \"A:\"";

static const char kHelpClear[] =
	"CLEAR\n"
	"ERASE     (alias)\n"
	"\n"
	"Clear variables. The program text is kept (see NEW).";

static const char kHelpPause[] =
	"PAUSE milliseconds\n"
	"\n"
	"Wait for the given number of milliseconds.\n"
	"\n"
	"Example:  PAUSE 100";

static const char kHelpSeek[] =
	"SEEK #n, pos\n"
	"\n"
	"Set the read/write position of open file n.\n"
	"\n"
	"Example:  SEEK #1, 0";

static const char kHelpEnd[] =
	"END\n"
	"END IF / ENDIF\n"
	"END SELECT\n"
	"END SUB\n"
	"END FUNCTION\n"
	"\n"
	"END stops a running program.\n"
	"END IF closes a multiline IF (see HELP IF).\n"
	"END SELECT closes SELECT CASE.\n"
	"END SUB / END FUNCTION close a SUB or FUNCTION.";

static const char kHelpCall[] =
	"CALL name\n"
	"\n"
	"Call a SUB (or FUNCTION) by name. Arguments are not\n"
	"implemented. See HELP SUB.\n"
	"\n"
	"Example:\n"
	"  10 SUB HI\n"
	"  20 PRINT 42\n"
	"  30 END SUB\n"
	"  40 CALL HI";

static const char kHelpFor[] =
	"FOR var = start TO end [STEP step]\n"
	"  statements\n"
	"NEXT [var]\n"
	"\n"
	"Count var from start to end. step defaults to 1.\n"
	"NEXT without a name uses the innermost FOR.\n"
	"Use numbered program lines; RUN executes the loop.\n"
	"\n"
	"Example:\n"
	"  10 FOR I=1 TO 3\n"
	"  20 PRINT I\n"
	"  30 NEXT I\n"
	"  RUN\n"
	"\n"
	"  10 FOR I=5 TO 1 STEP -2\n"
	"  20 PRINT I\n"
	"  30 NEXT I";

static const char kHelpWhile[] =
	"WHILE condition\n"
	"  statements\n"
	"WEND\n"
	"\n"
	"Repeat while condition is non-zero.\n"
	"\n"
	"Example:\n"
	"  10 I=0\n"
	"  20 WHILE I<3\n"
	"  30 I=I+1\n"
	"  40 PRINT I\n"
	"  50 WEND\n"
	"  RUN";

static const char kHelpDo[] =
	"DO [WHILE condition | UNTIL condition]\n"
	"  statements\n"
	"  EXIT DO\n"
	"LOOP [WHILE condition | UNTIL condition]\n"
	"\n"
	"Repeat a block. Test on DO and/or LOOP.\n"
	"EXIT DO jumps to the statement after LOOP.\n"
	"\n"
	"Example:\n"
	"  10 I=0\n"
	"  20 DO\n"
	"  30 I=I+1\n"
	"  40 PRINT I\n"
	"  50 LOOP UNTIL I=3\n"
	"  RUN";

static const char kHelpIf[] =
	"IF condition THEN statement [ELSE statement]\n"
	"IF condition THEN\n"
	"  statements\n"
	"ELSEIF condition THEN\n"
	"  statements\n"
	"ELSE\n"
	"  statements\n"
	"ENDIF\n"
	"\n"
	"Single-line IF runs the THEN (or ELSE) statement.\n"
	"A THEN at end of line starts a multiline IF closed\n"
	"by ENDIF (or END IF).\n"
	"\n"
	"Example:\n"
	"  10 X=1\n"
	"  20 IF X>0 THEN PRINT \"YES\" ELSE PRINT \"NO\"\n"
	"  RUN\n"
	"\n"
	"  10 X=0\n"
	"  20 IF X>0 THEN\n"
	"  30 PRINT \"YES\"\n"
	"  40 ELSE\n"
	"  50 PRINT \"NO\"\n"
	"  60 ENDIF";

static const char kHelpDim[] =
	"DIM name[(d1[,d2...])] [AS INTEGER|FLOAT|STRING] [, ...]\n"
	"\n"
	"Declare a variable or array. Suffixes $ % ! also set\n"
	"type. Bounds are inclusive upper bounds. OPTION BASE\n"
	"0 (default) or 1 sets the lower bound and must come\n"
	"before DIM.\n"
	"\n"
	"Example:\n"
	"  DIM A(2)\n"
	"  A(0)=10 : A(1)=20 : A(2)=12\n"
	"  PRINT A(0)+A(1)+A(2)\n"
	"\n"
	"  DIM N AS INTEGER\n"
	"  N=6 : PRINT N*7";

static const char kHelpConst[] =
	"CONST name = expr\n"
	"\n"
	"Define a constant. Suffixes $ % ! set the type.\n"
	"\n"
	"Example:\n"
	"  CONST MAX=21\n"
	"  PRINT MAX*2";

static const char kHelpData[] =
	"DATA item [, item...]\n"
	"READ var [, var...]\n"
	"RESTORE [line]\n"
	"\n"
	"DATA stores values in the program. READ assigns the\n"
	"next items. RESTORE rewinds to the first DATA, or to\n"
	"the first DATA at or after line.\n"
	"\n"
	"Example:\n"
	"  10 DATA 10,20,12\n"
	"  20 READ A,B,C\n"
	"  30 PRINT A+B+C\n"
	"  RUN";

static const char kHelpSub[] =
	"SUB name\n"
	"  statements\n"
	"END SUB\n"
	"\n"
	"FUNCTION name\n"
	"  statements\n"
	"END FUNCTION\n"
	"\n"
	"CALL name\n"
	"\n"
	"Define a subroutine. CALL name runs it. Arguments\n"
	"and calling FUNCTION from an expression are not\n"
	"implemented; use CALL for both SUB and FUNCTION.\n"
	"\n"
	"Example:\n"
	"  10 SUB HI\n"
	"  20 PRINT 6*7\n"
	"  30 END SUB\n"
	"  40 CALL HI\n"
	"  RUN";

static const char kHelpGoto[] =
	"GOTO line\n"
	"\n"
	"Jump to a numbered program line.\n"
	"\n"
	"Example:\n"
	"  10 GOTO 30\n"
	"  20 PRINT \"NO\"\n"
	"  30 PRINT 42\n"
	"  RUN";

static const char kHelpGosub[] =
	"GOSUB line\n"
	"RETURN\n"
	"\n"
	"Call a numbered subroutine. RETURN continues after\n"
	"the GOSUB. Nested GOSUB is limited to 16 levels.\n"
	"\n"
	"Example:\n"
	"  10 GOSUB 50\n"
	"  20 END\n"
	"  50 PRINT 6*7\n"
	"  60 RETURN\n"
	"  RUN";

static const char kHelpSelect[] =
	"SELECT CASE expr\n"
	"CASE value [, value...]\n"
	"CASE IS relop expr\n"
	"CASE ELSE\n"
	"END SELECT\n"
	"\n"
	"Choose a branch matching expr. relop is =, <>, <, >, <=, >=.\n"
	"\n"
	"Example:\n"
	"  10 N=2\n"
	"  20 SELECT CASE N\n"
	"  30 CASE 1 : PRINT \"ONE\"\n"
	"  40 CASE 2 : PRINT \"TWO\"\n"
	"  50 CASE ELSE : PRINT \"OTHER\"\n"
	"  60 END SELECT\n"
	"  RUN";

static const char kHelpLet[] =
	"LET var = expr\n"
	"var = expr\n"
	"\n"
	"Assign to a variable. LET is optional.\n"
	"Types: default float, var% integer, var$ string.\n"
	"\n"
	"Example:\n"
	"  LET A=21\n"
	"  B%=2\n"
	"  PRINT A*B%";

static const char kHelpRem[] =
	"REM comment\n"
	"' comment\n"
	"\n"
	"A remark. The rest of the statement is ignored.\n"
	"\n"
	"Example:  REM this is ignored\n"
	"          PRINT 1 ' trailing comment";

static const char kHelpFunctions[] =
	"Functions (use in expressions; HELP FUNCTIONS)\n"
	"\n"
	"Strings: LEN ASC CHR$ STR$ VAL LEFT$ RIGHT$ MID$\n"
	"  UCASE$ LCASE$ SPACE$ STRING$ INSTR HEX$ OCT$ BIN$\n"
	"Math: ABS INT FIX SQR/SQRT SIN COS TAN ATN/ATAN\n"
	"  ATN2 RND SGN EXP LOG PI\n"
	"Graphics: RGB(r,g,b)|RGB(\"NAME\")  PIXEL(x,y)\n"
	"  MM.HRES MM.VRES MM.INFO(MODE)\n"
	"Files: EOF(#n) LOF(#n) CWD$ INPUT$(n,#fn)\n"
	"Other: PLAYING() DATE$ TIME$ TIMER\n"
	"  MM.VER MM.DEVICE$ MM.CMDLINE$\n"
	"\n"
	"OPTION ANGLE DEGREES makes SIN/COS/TAN/ATN use degrees.\n"
	"\n"
	"Example:  PRINT LEFT$(\"MMBASIC\",2)\n"
	"          PRINT RGB(255,0,0)";

static const help_topic kTopics[] = {
	{ "HELP",        HELP_CMD,  kHelpHelp },
	{ "CLS",         HELP_CMD,  kHelpCls },
	{ "PRINT",       HELP_CMD,  kHelpPrint },
	{ "PIXEL",       HELP_CMD,  kHelpPixel },
	{ "LINE",        HELP_CMD,  kHelpLine },
	{ "LINE INPUT",  HELP_CMD,  kHelpLineInput },
	{ "BOX",         HELP_CMD,  kHelpBox },
	{ "CIRCLE",      HELP_CMD,  kHelpCircle },
	{ "RBOX",        HELP_CMD,  kHelpRbox },
	{ "ARC",         HELP_CMD,  kHelpArc },
	{ "TRIANGLE",    HELP_CMD,  kHelpTriangle },
	{ "POLYGON",     HELP_CMD,  kHelpPolygon },
	{ "TEXT",        HELP_CMD,  kHelpText },
	{ "FONT",        HELP_CMD,  kHelpFont },
	{ "COLOUR",      HELP_CMD,  kHelpColour },
	{ "MODE",        HELP_CMD,  kHelpMode },
	{ "PAGE",        HELP_CMD,  kHelpPage },
	{ "BLIT",        HELP_CMD,  kHelpBlit },
	{ "PLAY",        HELP_CMD,  kHelpPlay },
	{ "EDIT",        HELP_CMD,  kHelpEdit },
	{ "DIR",         HELP_CMD,  kHelpDir },
	{ "FILES",       HELP_CMD,  kHelpFiles },
	{ "OPEN",        HELP_CMD,  kHelpOpen },
	{ "CLOSE",       HELP_CMD,  kHelpClose },
	{ "LOAD",        HELP_CMD,  kHelpLoad },
	{ "SAVE",        HELP_CMD,  kHelpSave },
	{ "RUN",         HELP_CMD,  kHelpRun },
	{ "NEW",         HELP_CMD,  kHelpNew },
	{ "LIST",        HELP_CMD,  kHelpList },
	{ "INPUT",       HELP_CMD,  kHelpInput },
	{ "OPTION",      HELP_CMD,  kHelpOption },
	{ "CHDIR",       HELP_CMD,  kHelpChdir },
	{ "MKDIR",       HELP_CMD,  kHelpMkdir },
	{ "RMDIR",       HELP_CMD,  kHelpRmdir },
	{ "COPY",        HELP_CMD,  kHelpCopy },
	{ "RENAME",      HELP_CMD,  kHelpRename },
	{ "KILL",        HELP_CMD,  kHelpKill },
	{ "DRIVE",       HELP_CMD,  kHelpDrive },
	{ "CLEAR",       HELP_CMD,  kHelpClear },
	{ "PAUSE",       HELP_CMD,  kHelpPause },
	{ "SEEK",        HELP_CMD,  kHelpSeek },
	{ "END",         HELP_CMD,  kHelpEnd },
	{ "CALL",        HELP_CMD,  kHelpCall },
	{ "FUNCTIONS",   HELP_CMD,  kHelpFunctions },
	{ "FOR",         HELP_LANG, kHelpFor },
	{ "WHILE",       HELP_LANG, kHelpWhile },
	{ "DO",          HELP_LANG, kHelpDo },
	{ "IF",          HELP_LANG, kHelpIf },
	{ "DIM",         HELP_LANG, kHelpDim },
	{ "CONST",       HELP_LANG, kHelpConst },
	{ "DATA",        HELP_LANG, kHelpData },
	{ "SUB",         HELP_LANG, kHelpSub },
	{ "GOTO",        HELP_LANG, kHelpGoto },
	{ "GOSUB",       HELP_LANG, kHelpGosub },
	{ "SELECT CASE", HELP_LANG, kHelpSelect },
	{ "LET",         HELP_LANG, kHelpLet },
	{ "REM",         HELP_LANG, kHelpRem },
};

static const struct {
	const char *alias;
	const char *canon;
} kAlias[] = {
	{ "COLOR",        "COLOUR" },
	{ "NAME",         "RENAME" },
	{ "ERASE",        "CLEAR" },
	{ "?",            "PRINT" },
	{ "LINEINPUT",    "LINE INPUT" },
	{ "NEXT",         "FOR" },
	{ "WEND",         "WHILE" },
	{ "LOOP",         "DO" },
	{ "EXIT",         "DO" },
	{ "EXIT DO",      "DO" },
	{ "ENDIF",        "IF" },
	{ "END IF",       "IF" },
	{ "ELSE",         "IF" },
	{ "ELSEIF",       "IF" },
	{ "THEN",         "IF" },
	{ "SELECT",       "SELECT CASE" },
	{ "CASE",         "SELECT CASE" },
	{ "END SELECT",   "SELECT CASE" },
	{ "READ",         "DATA" },
	{ "RESTORE",      "DATA" },
	{ "FUNCTION",     "SUB" },
	{ "END SUB",      "SUB" },
	{ "END FUNCTION", "SUB" },
	{ "RETURN",       "GOSUB" },
	{ "FUNCTIONS",    "FUNCTIONS" },
};

static const char *resolve_alias(const char *q)
{
	int i, n = (int)(sizeof(kAlias) / sizeof(kAlias[0]));
	for (i = 0; i < n; i++)
		if (mmb_keyword_eq(kAlias[i].alias, q))
			return kAlias[i].canon;
	return q;
}

static const help_topic *find_by_name(const char *q)
{
	int i, n = (int)(sizeof(kTopics) / sizeof(kTopics[0]));
	for (i = 0; i < n; i++)
		if (mmb_keyword_eq(kTopics[i].name, q))
			return &kTopics[i];
	return 0;
}

static void first_word(const char *q, char *dst, int dstsz)
{
	int n = 0;
	while (*q && *q != ' ' && n < dstsz - 1)
		dst[n++] = *q++;
	dst[n] = 0;
}

static const help_topic *find_topic(const char *q)
{
	const help_topic *t;
	char word[40];
	const char *canon;

	canon = resolve_alias(q);
	t = find_by_name(canon);
	if (t)
		return t;
	first_word(q, word, sizeof(word));
	if (word[0] && !mmb_keyword_eq(word, q))
	{
		canon = resolve_alias(word);
		t = find_by_name(canon);
		if (t)
			return t;
	}
	return 0;
}

static int at_end(void)
{
	mmb_skip_sp();
	return *G.p == 0 || *G.p == ':' || *G.p == '\'';
}

static void read_topic(char *dst, int dstsz)
{
	int n = 0;
	mmb_skip_sp();
	if (*G.p == '?')
	{
		dst[0] = '?';
		dst[1] = 0;
		G.p++;
		return;
	}
	while (*G.p && *G.p != ':' && *G.p != '\'' && n < dstsz - 2)
	{
		if (*G.p == ' ' || *G.p == '\t')
		{
			mmb_skip_sp();
			if (*G.p && *G.p != ':' && *G.p != '\'' && n > 0 && dst[n - 1] != ' ')
				dst[n++] = ' ';
			continue;
		}
		{
			char c = *G.p++;
			if (c >= 'a' && c <= 'z')
				c = (char)(c - 32);
			dst[n++] = c;
		}
	}
	while (n > 0 && dst[n - 1] == ' ')
		n--;
	dst[n] = 0;
}

void mmb_cmd_help(void)
{
	char topic[48];
	const help_topic *t;

	if (at_end())
	{
		mmb_out(kIndexCommands);
		return;
	}
	if (mmb_match("BASIC"))
	{
		if (at_end())
		{
			mmb_out(kIndexBasic);
			return;
		}
	}
	read_topic(topic, sizeof(topic));
	if (!topic[0])
	{
		mmb_out(kIndexCommands);
		return;
	}
	t = find_topic(topic);
	if (!t)
	{
		mmb_out("Unknown topic: ");
		mmb_out(topic);
		mmb_out("\nType HELP for commands, HELP BASIC for language.");
		return;
	}
	mmb_out(t->text);
}
