# Command and function gap analysis

Generated: 2026-09-16

Inventories:

- **mmCore** — commands in `mmbasic/src/core.c` (`try_tok_cmd`) and functions in `mmbasic/src/expr.c` (`mmb_try_function`), plus noted compound forms.
- **MMBasic** — PicoMite `picomite-fork/AllCommands.h` command and function token tables (reference submodule).
- **QuickBasic** — keyword list derived from https://gamma.zem.fi/~fis/qb.html (see `scripts/data/quickbasic_keywords.txt`).

An `x` means the symbol is available in that inventory. Subcommands may appear as separate rows. This file is Markdown documentation only and is **not** compiled into the HELP firmware image.

Regenerate with `scripts/gap_analysis.py`. Do not open GitHub issues from this file unless explicitly asked.

## Summary counts

| Inventory | Symbols |
|-----------|--------:|
| mmCore | 202 |
| MMBasic (PicoMite) | 287 |
| QuickBasic | 220 |
| Union | 448 |
| In PicoMite / QB but not mmCore | 246 |
| In mmCore only | 45 |

## Control Flow

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `CASE` | Selects among CASE branch paths | x | x | x |
| `CASE ELSE` | Selects among CASE branch paths | x | x | x |
| `CHAIN` | Chains into another program file |  | x | x |
| `CONTINUE` | Skips ahead to next iteration | x | x |  |
| `CONTINUE DO` | Skips ahead to next iteration | x | x |  |
| `CONTINUE FOR` | Skips ahead to next iteration | x | x |  |
| `DO` | Provides general DO LOOP form | x | x | x |
| `ELSE` | Branches with IF THEN blocks | x | x | x |
| `ELSE IF` | Branches with IF THEN blocks | x | x | x |
| `ELSEIF` | Branches with IF THEN blocks | x | x | x |
| `END` | Ends the running program now | x | x | x |
| `END IF` | Branches with IF THEN blocks | x | x | x |
| `END SELECT` | Selects among CASE branch paths | x | x | x |
| `ENDIF` | Branches with IF THEN blocks | x | x | x |
| `EXIT` | Leaves current loop or routine | x | x | x |
| `EXIT DO` | Leaves current loop or routine | x | x | x |
| `EXIT FOR` | Leaves current loop or routine | x | x | x |
| `EXIT FUNCTION` | Leaves current loop or routine | x | x | x |
| `EXIT SUB` | Leaves current loop or routine | x | x | x |
| `FOR` | Runs counted loop with STEP | x | x | x |
| `GOSUB` | Calls subroutine at named label | x | x | x |
| `GOTO` | Jumps execution to named label | x | x | x |
| `IF` | Branches with IF THEN blocks | x | x | x |
| `LOOP` | Provides general DO LOOP form | x | x | x |
| `NEXT` | Runs counted loop with STEP | x | x | x |
| `ON` | Handles ON events and jumps | x | x | x |
| `ON ERROR` | Handles ON events and jumps | x | x | x |
| `ON GOSUB` | Handles ON events and jumps | x | x | x |
| `ON GOTO` | Handles ON events and jumps | x | x | x |
| `REM` | Comments out remaining program line | x | x | x |
| `RESUME` | Resumes after trapped runtime error |  |  | x |
| `RETURN` | Calls subroutine at named label | x | x | x |
| `SELECT` | Selects among CASE branch paths | x |  | x |
| `SELECT CASE` | Selects among CASE branch paths | x | x | x |
| `STOP` | Ends the running program now |  |  | x |
| `SYSTEM` | Ends the running program now |  |  | x |
| `THEN` | Branches with IF THEN blocks |  |  | x |
| `TRACE` | Turns execution tracing on off |  | x |  |
| `TROFF` | Turns execution tracing on off |  |  | x |
| `TRON` | Turns execution tracing on off |  |  | x |
| `WEND` | Loops while condition remains true | x |  | x |
| `WHILE` | Loops while condition remains true | x | x | x |

## Variables and Types

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `BOUND` | Returns lower or upper bound | x | x |  |
| `CLEAR` | Deletes variables and array storage | x | x | x |
| `COMMON` | Shares variables across procedure scopes |  |  | x |
| `CONST` | Declares named compile time constants | x | x | x |
| `DATA` | Embeds DATA values inside program | x | x | x |
| `DEC` | Decrements a numeric variable value | x |  |  |
| `DEFTYPE` | Sets default type by letter |  |  | x |
| `DIM` | Declares variables and array storage | x | x | x |
| `END TYPE` | Defines a user structured type | x | x | x |
| `ERASE` | Deletes variables and array storage | x | x | x |
| `INC` | Increments a numeric variable value | x | x |  |
| `LBOUND` | Returns lower or upper bound |  |  | x |
| `LET` | Assigns expression result to variable | x | x | x |
| `LOCAL` | Declares procedure local variable names | x | x |  |
| `OPTION BASE` | Sets default lowest array index |  |  | x |
| `READ` | Embeds DATA values inside program | x | x | x |
| `REDIM` | Declares variables and array storage |  | x | x |
| `RESTORE` | Embeds DATA values inside program | x | x | x |
| `SHARED` | Shares variables across procedure scopes |  |  | x |
| `STATIC` | Declares static local variable storage | x | x | x |
| `SWAP` | Swaps values of two variables |  |  | x |
| `TYPE` | Defines a user structured type | x | x | x |
| `UBOUND` | Returns lower or upper bound |  |  | x |

## Procedures

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `CALL` | Invokes a named SUB routine | x | x | x |
| `DECLARE` | Declares procedure name in advance |  |  | x |
| `DEF FN` | Defines a single line function |  |  | x |
| `END FUNCTION` | Defines a callable FUNCTION routine | x | x | x |
| `END SUB` | Defines a callable SUB routine | x | x | x |
| `FUNCTION` | Defines a callable FUNCTION routine | x | x | x |
| `RUN` | Runs a program or procedure | x | x | x |
| `SHELL` | Runs an external shell command |  |  | x |
| `SUB` | Defines a callable SUB routine | x | x | x |

## Console and Text I/O

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `BEEP` | Sounds a short speaker beep |  |  | x |
| `CLS` | Clears the display graphics screen | x | x | x |
| `CSRLIN` | Returns current text cursor column |  |  | x |
| `INKEY$` | Reads keyboard without waiting pause | x | x | x |
| `INP` | Reads or writes hardware ports |  |  | x |
| `INPUT` | Reads values from the console | x | x | x |
| `KEY` | Assigns strings to keyboard macros |  |  | x |
| `KEYDOWN` | Tests whether keyboard key down | x | x |  |
| `LINE INPUT` | Reads values from the console | x | x | x |
| `LOCATE` | Moves the console text cursor | x |  | x |
| `LPOS` | Returns current text cursor column |  |  | x |
| `LPRINT` | Writes text to printer stream |  |  | x |
| `OUT` | Reads or writes hardware ports |  | x | x |
| `PAUSE` | Waits for a timed delay | x | x |  |
| `POS` | Returns current text cursor column | x | x | x |
| `PRINT` | Writes text to the console | x | x | x |
| `SLEEP` | Waits for a timed delay |  |  | x |
| `SPACE$` | Prints spaces or tab columns | x | x | x |
| `SPC` | Prints spaces or tab columns |  |  | x |
| `TAB` | Prints spaces or tab columns | x | x | x |
| `VIEW` | Sets printable text viewport window |  |  | x |
| `VSYNC_WAIT` | Waits for a timed delay | x |  |  |
| `WAIT` | Waits for a timed delay |  | x | x |
| `WIDTH` | Sets console output text width |  |  | x |

## String Operations

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `ASC` | Returns ASCII code from character | x | x | x |
| `BASE$` | Converts values between numeric bases |  | x |  |
| `BIN$` | Formats number as binary string | x | x |  |
| `BIN2STR$` | Converts binary buffer into string |  | x |  |
| `CAT` | Appends text onto string variable | x | x |  |
| `CHR$` | Builds character from ASCII code | x | x | x |
| `DATETIME$` | Formats combined date time string |  | x |  |
| `DAY$` | Returns weekday name as string |  | x |  |
| `DIR$` | Returns directory listing as string |  | x |  |
| `FIELD` | Defines fielded string inside record |  |  | x |
| `FIELD$` | Extracts delimited field from string |  | x |  |
| `FORMAT$` | Formats number into custom string | x | x |  |
| `HEX$` | Formats number as hexadecimal string | x | x | x |
| `INSTR` | Finds starting position of substring | x | x | x |
| `LCASE$` | Converts string characters to lowercase | x | x | x |
| `LEFT$` | Returns leftmost characters from string | x | x | x |
| `LEN` | Returns string length in characters | x | x | x |
| `LGETSTR$` | Gets substring from long string |  | x |  |
| `LONGSTRING` | Performs long string buffer operations |  | x |  |
| `LSET` | Left or right justifies string |  |  | x |
| `LTRIM$` | Removes spaces from string edges |  |  | x |
| `MID$` | Extracts or replaces string substring | x | x | x |
| `OCT$` | Formats number as octal string | x |  | x |
| `RIGHT$` | Returns rightmost characters from string | x | x | x |
| `RSET` | Left or right justifies string |  |  | x |
| `RTRIM$` | Removes spaces from string edges |  |  | x |
| `SCHANGE$` | Searches and replaces string text |  | x |  |
| `SORT` | Sorts values stored in array | x | x |  |
| `STR$` | Converts number into decimal string | x | x | x |
| `STRING$` | Builds string of repeated characters | x | x | x |
| `TRIM$` | Trims spaces from both sides |  | x |  |
| `UCASE$` | Converts string characters to uppercase | x | x | x |
| `VAL` | Converts string into numeric value | x | x | x |

## Math Functions

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `ABS` | Returns absolute value of number | x | x | x |
| `ACOS` | Returns inverse sine or cosine | x | x |  |
| `ACS` | Returns inverse sine or cosine | x | x |  |
| `ASIN` | Returns inverse sine or cosine | x | x |  |
| `ASN` | Returns inverse sine or cosine | x | x |  |
| `ATAN` | Returns arctangent of numeric value | x | x | x |
| `ATAN2` | Returns arctangent of numeric value |  | x |  |
| `ATN` | Returns arctangent of numeric value | x | x | x |
| `ATN2` | Returns arctangent of numeric value | x |  |  |
| `CDBL` | Truncates number value toward zero |  |  | x |
| `CHOICE` | Chooses value based on condition | x | x |  |
| `CINT` | Truncates number value toward zero | x | x | x |
| `CLNG` | Truncates number value toward zero |  |  | x |
| `COS` | Returns cosine of angle value | x | x | x |
| `CSNG` | Truncates number value toward zero |  |  | x |
| `CVD` | Converts packed numeric binary forms |  |  | x |
| `CVDMBF` | Converts packed numeric binary forms |  |  | x |
| `CVI` | Converts packed numeric binary forms |  |  | x |
| `CVN` | Converts packed numeric binary forms |  |  | x |
| `CVS` | Converts packed numeric binary forms |  |  | x |
| `CVSMBF` | Converts packed numeric binary forms |  |  | x |
| `DEG` | Converts between degrees and radians | x | x |  |
| `EVAL` | Evaluates string text as expression | x | x |  |
| `EXP` | Raises e to given power | x | x | x |
| `FIX` | Truncates number value toward zero | x | x | x |
| `INT` | Truncates number value toward zero | x | x | x |
| `LOG` | Returns natural logarithm of number | x | x | x |
| `MATH` | Performs array and matrix math | x | x |  |
| `MAX` | Returns maximum among given arguments | x | x |  |
| `MIN` | Returns minimum among given arguments | x | x |  |
| `MKD$` | Converts packed numeric binary forms |  |  | x |
| `MKDMBF$` | Converts packed numeric binary forms |  |  | x |
| `MKI$` | Converts packed numeric binary forms |  |  | x |
| `MKN$` | Converts packed numeric binary forms |  |  | x |
| `MKS$` | Converts packed numeric binary forms |  |  | x |
| `MKSMBF$` | Converts packed numeric binary forms |  |  | x |
| `MOD` | Computes integer modulo remainder value |  |  | x |
| `PI` | Returns mathematical constant value pi | x | x |  |
| `RAD` | Converts between degrees and radians | x | x |  |
| `RANDOMIZE` | Returns or seeds random numbers | x | x | x |
| `RND` | Returns or seeds random numbers | x | x | x |
| `SGN` | Returns algebraic sign of number | x | x | x |
| `SIN` | Returns sine of angle value | x | x | x |
| `SQR` | Returns square root of number | x | x | x |
| `SQRT` | Returns square root of number | x | x | x |
| `TAN` | Returns tangent of angle value | x | x | x |

## File System

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `CHDIR` | Changes the current working directory | x | x | x |
| `COPY` | Copies a file between paths | x | x |  |
| `CWD$` | Returns current working directory path | x | x |  |
| `DEL` | Deletes a file from storage | x |  |  |
| `DIR` | Lists files inside a directory | x |  |  |
| `DRIVE` | Selects the active drive letter | x | x |  |
| `FILES` | Opens fullscreen interactive file browser | x | x | x |
| `FLASH` | Manages on device flash storage |  | x |  |
| `KILL` | Deletes a file from storage | x | x | x |
| `LIBRARY` | Loads or saves library modules |  | x |  |
| `LS` | Lists files inside a directory | x |  |  |
| `MKDIR` | Creates a new filesystem directory | x | x | x |
| `MV` | Renames or moves a file | x |  |  |
| `NAME` | Renames or moves a file | x |  | x |
| `PACKAGE` | Installs or manages software packages | x |  |  |
| `RENAME` | Renames or moves a file | x | x | x |
| `RM` | Deletes a file from storage | x |  |  |
| `RMDIR` | Removes an empty filesystem directory | x | x | x |
| `XFER` | Copies a file between paths | x |  |  |
| `XMODEM` | Transfers files using XMODEM protocol |  | x |  |
| `YMODEM` | Transfers files using XMODEM protocol |  | x |  |

## File I/O

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `CLOSE` | Closes an open file number | x | x | x |
| `EOF` | Tests whether file reached EOF | x | x | x |
| `FILEATTR` | Returns attributes for open file |  |  | x |
| `FLUSH` | Flushes pending buffered file output |  | x |  |
| `FREEFILE` | Returns next unused file number |  |  | x |
| `GET` | Gets binary data from file |  |  | x |
| `INPUT$` | Reads characters from open file | x | x | x |
| `IOCTL` | Sends IOCTL device control strings |  |  | x |
| `IOCTL$` | Sends IOCTL device control strings |  |  | x |
| `LOAD` | Loads program text from storage | x | x |  |
| `LOC` | Returns current open file position | x | x | x |
| `LOCK` | Locks or unlocks file records |  |  | x |
| `LOF` | Returns length of open file | x | x | x |
| `OPEN` | Opens a file or device | x | x | x |
| `OPEN COM` | Opens a file or device |  |  | x |
| `PUT` | Puts binary data into file |  |  | x |
| `RESET` | Resets and closes open files |  |  | x |
| `SAVE` | Saves program text to storage | x | x |  |
| `SEEK` | Seeks to given file position | x | x | x |
| `UNLOCK` | Locks or unlocks file records |  |  | x |
| `WRITE` | Writes values into open file |  |  | x |

## Time and Events

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `COM` | Traps serial communications event signals |  |  | x |
| `DATE$` | Returns current calendar date string | x | x | x |
| `ERDEV` | Reports low level device errors |  |  | x |
| `ERDEV$` | Reports low level device errors |  |  | x |
| `ERL` | Reports error number and line |  |  | x |
| `ERR` | Reports error number and line |  |  | x |
| `ERROR` | Reports error number and line | x | x | x |
| `IRETURN` | Returns from active interrupt handler |  | x |  |
| `ON COM` | Traps serial communications event signals | x | x | x |
| `ON KEY` | Traps programmable KEY event signals | x | x | x |
| `ON PEN` | Reads light pen input state | x | x | x |
| `ON PLAY` | Traps background PLAY event signals | x | x | x |
| `ON STRIG` | Reads joystick and button state | x | x | x |
| `ON TIMER` | Traps periodic timer event signals | x | x | x |
| `PEN` | Reads light pen input state |  |  | x |
| `SETTICK` | Configures periodic SETTICK interrupt timer | x | x |  |
| `STICK` | Reads joystick and button state |  |  | x |
| `STRIG` | Reads joystick and button state |  |  | x |
| `TIME$` | Returns current clock time string | x | x | x |
| `TIMER` | Reads millisecond resolution system timer | x | x | x |
| `WATCHDOG` | Controls hardware watchdog timer behavior |  | x |  |

## System and Options

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `@` | Legacy language or system keyword |  | x |  |
| `AUTOSAVE` | Autosaves current program edit buffer |  | x |  |
| `BIT` | Legacy language or system keyword |  | x |  |
| `BYTE` | Legacy language or system keyword |  | x |  |
| `CPU` | Reboots the host hardware machine |  | x |  |
| `CREDITS` | Shows copyright and credit notices | x |  |  |
| `CSUB` | Defines embedded native C subroutine |  | x |  |
| `DEFINEFONT` | Defines embedded native C subroutine |  | x |  |
| `EDIT` | Opens fullscreen program source editor | x | x |  |
| `EDIT FILE` | Opens fullscreen program source editor |  | x |  |
| `END CSUB` | Defines embedded native C subroutine | x | x | x |
| `END DEFINEFONT` | Defines embedded native C subroutine | x | x | x |
| `ENVIRON` | Reads or writes environment variables |  |  | x |
| `ENVIRON$` | Reads or writes environment variables |  |  | x |
| `EPOCH` | Legacy language or system keyword |  | x |  |
| `EXECUTE` | Executes string contents as code |  | x |  |
| `FACTORY` | Restores factory default option values | x |  |  |
| `FACTORY RESET` | Restores factory default option values | x |  |  |
| `FACTORY_RESET` | Restores factory default option values | x |  |  |
| `FLAG` | Legacy language or system keyword |  | x |  |
| `FRE` | Returns amount of free memory |  |  | x |
| `GETSCANLINE` | Legacy language or system keyword |  | x |  |
| `HELP` | Opens interactive on device help | x | x |  |
| `IHELP` | Opens interactive on device help | x |  |  |
| `IN` | Legacy language or system keyword |  | x |  |
| `IRQ` | Legacy language or system keyword |  | x |  |
| `IRQ CLEAR` | Legacy language or system keyword |  | x |  |
| `IRQ NEXT` | Legacy language or system keyword |  | x |  |
| `IRQ NOWAIT` | Legacy language or system keyword |  | x |  |
| `IRQ PREV` | Legacy language or system keyword |  | x |  |
| `IRQ SET` | Legacy language or system keyword |  | x |  |
| `IRQ WAIT` | Legacy language or system keyword |  | x |  |
| `JMP` | Legacy language or system keyword |  | x |  |
| `LCOMPARE` | Legacy language or system keyword |  | x |  |
| `LGETBYTE` | Legacy language or system keyword |  | x |  |
| `LINPUT` | Legacy language or system keyword |  | x |  |
| `LINSTR` | Legacy language or system keyword |  | x |  |
| `LIST` | Lists stored program source lines | x | x |  |
| `LIST FILES` | Legacy language or system keyword | x |  |  |
| `LIST TYPE` | Legacy language or system keyword | x |  |  |
| `LLEN` | Legacy language or system keyword |  | x |  |
| `LMID` | Legacy language or system keyword |  | x |  |
| `MEMORY` | Shows current memory usage statistics | x | x |  |
| `MOV` | Legacy language or system keyword |  | x |  |
| `NEW` | Clears loaded program from memory | x | x | x |
| `NOP` | Legacy language or system keyword |  | x |  |
| `OPTION` | Sets persistent interpreter option values | x | x | x |
| `OPTIONS` | Sets persistent interpreter option values | x |  |  |
| `PEEK` | Reads raw bytes from memory |  | x | x |
| `PIN` | Legacy language or system keyword |  | x |  |
| `POKE` | Writes raw bytes into memory |  | x | x |
| `PORT` | Legacy language or system keyword |  | x |  |
| `PULL` | Legacy language or system keyword |  | x |  |
| `PUSH` | Legacy language or system keyword |  | x |  |
| `REBOOT` | Reboots the host hardware machine | x |  |  |
| `RESTART` | Reboots the host hardware machine | x |  |  |
| `SET` | Legacy language or system keyword |  | x |  |
| `STR2BIN` | Legacy language or system keyword |  | x |  |
| `TOPBOTTOM` | Legacy language or system keyword |  | x |  |
| `UPDATE FIRMWARE` | Updates installed device firmware image |  | x |  |
| `VAR` | Saves or restores variable workspace |  | x |  |
| `WORDPAD` | Opens simple fullscreen text editor | x |  |  |
| `~` | Legacy language or system keyword |  | x |  |

## Network and Terminal

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `CONNECT` | Helps establish network link connection | x |  |  |
| `IPCONFIG` | Shows active IP network configuration | x |  |  |
| `TERM` | Opens serial or TCP terminal | x |  |  |
| `WEB` | Runs built-in web client utilities |  | x |  |

## Sound and Play

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `PLAY` | Plays tones or media files | x | x | x |
| `PLAYING` | Reports whether audio currently playing | x |  |  |
| `SOUND` | Plays simple PC speaker sounds |  |  | x |

## Graphics

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `ARC` | Draws arc between two angles | x | x |  |
| `ASTRO` | Draws animated starfield graphics effect |  | x |  |
| `BACKLIGHT` | Refreshes or adjusts display output |  | x |  |
| `BEZIER` | Draws Bezier curve graphics paths |  | x |  |
| `BITMAP` | Plots packed bitmap pattern image | x |  |  |
| `BOX` | Draws filled or outline box | x | x |  |
| `CAMERA` | Captures frames from camera device |  | x |  |
| `CIRCLE` | Draws circle outline or fill | x | x | x |
| `COLOR` | Sets foreground and background colours | x | x | x |
| `COLOUR` | Sets foreground and background colours | x | x | x |
| `COLOUR MAP` | Maps indexed palette colour values |  | x |  |
| `DRAW` | Runs DRAW graphics macro language |  |  | x |
| `DRAW3D` | Provides helpers for 3D drawing |  | x |  |
| `FILL` | Fills connected graphics region area |  | x |  |
| `FONT` | Selects active graphics text font | x | x |  |
| `FRAMEBUFFER` | Controls offscreen framebuffer page usage | x | x |  |
| `GUI` | Plots packed bitmap pattern image | x | x |  |
| `GUI BITMAP` | Plots packed bitmap pattern image | x | x |  |
| `IMAGE` | Loads and transforms image bitmaps | x |  |  |
| `LINE` | Draws a straight graphics line | x | x | x |
| `MANDELBROT` | Renders Mandelbrot fractal image output |  | x |  |
| `MAP` | Maps indexed palette colour values |  | x |  |
| `MODE` | Selects active screen video mode | x | x |  |
| `PAGE` | Selects active graphics drawing page | x |  |  |
| `PAINT` | Fills connected graphics region area |  |  | x |
| `PALETTE` | Maps indexed palette colour values |  |  | x |
| `PCOPY` | Copies pixels between screen pages |  |  | x |
| `PIXEL` | Sets or reads graphics pixel | x | x |  |
| `PMAP` | Maps coordinates between graphics spaces |  |  | x |
| `POINT` | Maps coordinates between graphics spaces |  |  | x |
| `POLYGON` | Draws multi point polygon shape | x | x |  |
| `PRESET` | Sets or clears graphics point |  |  | x |
| `PSET` | Sets or clears graphics point |  |  | x |
| `RAY` | Runs realtime raycaster render engine |  | x |  |
| `RBOX` | Draws rectangle with rounded corners | x | x |  |
| `REFRESH` | Refreshes or adjusts display output |  | x |  |
| `RESOLUTION` | Selects active screen video mode |  | x |  |
| `RGB` | Builds packed RGB colour value | x | x |  |
| `SCREEN` | Selects active screen video mode |  |  | x |
| `STAR` | Draws animated starfield graphics effect |  | x |  |
| `TEXT` | Draws text on graphics page | x | x |  |
| `TILE` | Controls tiled map display layers |  | x |  |
| `TILEMAP` | Controls tiled map display layers |  | x |  |
| `TRIANGLE` | Draws filled three point triangle | x | x |  |
| `TURTLE` | Runs LOGO style turtle graphics | x | x |  |
| `WINDOW` | Maps coordinates between graphics spaces |  |  | x |

## Sprites and Blit

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `BLIT` | Copies rectangular bit-block images keyword | x | x |  |
| `BLIT MEMORY` | Copies rectangular bit-block images keyword | x | x |  |
| `SPRITE` | Loads shows moves sprite objects | x | x |  |

## JSON and Structures

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `ARRAY ADD` | Slices inserts or fills arrays |  | x |  |
| `ARRAY INSERT` | Slices inserts or fills arrays |  | x |  |
| `ARRAY SET` | Slices inserts or fills arrays |  | x |  |
| `ARRAY SLICE` | Slices inserts or fills arrays |  | x |  |
| `JSON$` | Reads JSON path as string | x | x |  |
| `JSON_PARSE` | Parses JSON text into variables | x |  |  |
| `JSON_STRINGIFY$` | Serializes value into JSON text | x |  |  |
| `STRUCT` | Operates on structured record arrays | x | x |  |

## Environment (MM.*)

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `MM.CMDLINE$` | Returns startup command line string | x |  |  |
| `MM.DEVICE$` | Returns current device name string | x |  |  |
| `MM.HPOS` | Returns text cursor column position | x |  |  |
| `MM.HRES` | Returns horizontal resolution in pixels | x |  |  |
| `MM.INFO` | Queries MMBasic system info values | x | x |  |
| `MM.INFO$` | Queries MMBasic system info values | x |  |  |
| `MM.VER` | Returns firmware version string value | x |  |  |
| `MM.VPOS` | Returns text cursor row position | x |  |  |
| `MM.VRES` | Returns vertical resolution in pixels | x |  |  |

## Hardware and Devices

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `ADC` | Reads analog to digital converter |  | x |  |
| `BITSTREAM` | Generates timed digital bitstream output |  | x |  |
| `CALC` | Enables interactive calculator prompt mode |  | x |  |
| `CLICK` | Drives interactive on-screen GUI controls |  | x |  |
| `CMM2 LOAD` | Loads and runs CMM2 programs |  | x |  |
| `CMM2 RUN` | Loads and runs CMM2 programs |  | x |  |
| `CONFIGURE` | Configures selected hardware peripheral block |  | x |  |
| `CTRLVAL` | Drives interactive on-screen GUI controls |  | x |  |
| `DEVICE` | Runs peripheral device helper commands |  | x |  |
| `DISTANCE` | Reads values from distance sensor |  | x |  |
| `FLAGS` | Manipulates byte flag bit fields |  | x |  |
| `FM` | Controls FM radio peripheral helpers |  | x |  |
| `FRAME` | Provides helpers for frame timing |  | x |  |
| `GAMEPAD` | Reads state from gamepad device |  | x |  |
| `GPS` | Reads values from GPS helper |  | x |  |
| `HUMID` | Reads humidity from sensor device |  | x |  |
| `I2C` | Communicates over I2C device bus |  | x |  |
| `I2C2` | Communicates over I2C device bus |  | x |  |
| `I2CLCD` | Communicates over I2C device bus |  | x |  |
| `INTERRUPT` | Hooks interrupt to CSUB handler |  | x |  |
| `IR` | Handles infrared remote control codes |  | x |  |
| `KEYBOARD` | Configures attached keyboard device options |  | x |  |
| `KEYPAD` | Scans keys on matrix keypad |  | x |  |
| `LCD` | Drives attached LCD panel displays |  | x |  |
| `LOCATION` | Provides helpers for frame timing |  | x |  |
| `MOUSE` | Reads state from mouse device |  | x |  |
| `MSGBOX` | Drives interactive on-screen GUI controls |  | x |  |
| `ONESHOT` | Generates timed digital bitstream output |  | x |  |
| `ONEWIRE` | Communicates over one wire bus |  | x |  |
| `PIO` | Programs PIO hardware state machines |  | x |  |
| `PULSE` | Pulses selected GPIO pin output |  | x |  |
| `PULSIN` | Pulses selected GPIO pin output |  | x |  |
| `PWM` | Controls hardware PWM output channels |  | x |  |
| `RAM` | Accesses external PSRAM memory helpers |  | x |  |
| `RTC` | Controls onboard real time clock |  | x |  |
| `SERVO` | Controls positions of servo motors |  | x |  |
| `SETPIN` | Configures mode for GPIO pin |  | x |  |
| `SLEW` | Controls motion of stepper motors |  | x |  |
| `SPI` | Communicates over SPI device bus |  | x |  |
| `SPI2` | Communicates over SPI device bus |  | x |  |
| `STEPPER` | Controls motion of stepper motors |  | x |  |
| `SYNC` | Helps synchronize display timing signals |  | x |  |
| `TEMPR` | Communicates over one wire bus |  | x |  |
| `TEMPR START` | Communicates over one wire bus |  | x |  |
| `TMC22XX` | Controls motion of stepper motors |  | x |  |
| `TOUCH` | Reads values from touch panel |  | x |  |
| `WII` | Talks with Wii controller hardware |  | x |  |
| `WII CLASSIC` | Talks with Wii controller hardware |  | x |  |
| `WII NUNCHUCK` | Talks with Wii controller hardware |  | x |  |
| `WS2812` | Drives WS2812 addressable LED strips |  | x |  |

## QuickBasic Legacy

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |
|---|---|:---:|:---:|:---:|
| `AND` | Provides boolean logical operator keywords |  |  | x |
| `AS` | Supports counted loop step keywords |  |  | x |
| `BLOAD` | Loads or saves memory images |  |  | x |
| `BSAVE` | Loads or saves memory images |  |  | x |
| `DEF SEG` | Defines current PEEK POKE segment |  |  | x |
| `$DYNAMIC` | Controls optional source metacommand modes |  |  | x |
| `EQV` | Provides boolean logical operator keywords |  |  | x |
| `IMP` | Provides boolean logical operator keywords |  |  | x |
| `LPRINT USING` | Writes text to printer stream |  |  | x |
| `NOT` | Provides boolean logical operator keywords |  |  | x |
| `OFF` | Provides optional event OFF keyword |  |  | x |
| `OR` | Provides boolean logical operator keywords |  |  | x |
| `PRINT USING` | Writes text to the console |  |  | x |
| `SADD` | Returns pointers into variable storage |  |  | x |
| `$STATIC` | Controls optional source metacommand modes |  |  | x |
| `STEP` | Supports counted loop step keywords |  |  | x |
| `TO` | Supports counted loop step keywords |  |  | x |
| `VARPTR` | Returns pointers into variable storage |  |  | x |
| `VARPTR$` | Returns pointers into variable storage |  |  | x |
| `VARSEG` | Returns pointers into variable storage |  |  | x |
| `VIEW PRINT` | Sets printable text viewport window |  |  | x |
| `XOR` | Provides boolean logical operator keywords |  |  | x |
