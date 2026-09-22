#!/usr/bin/env python3
"""Compare mmCore, PicoMite MMBasic, and QuickBasic command/function inventories.

Writes docs/help/GAP_ANALYSIS_<YYYY-MM-DD>.md (Markdown only — never compiled
into help_data.c; gen_help.py loads *.txt exclusively).

Usage:
  scripts/gap_analysis.py              # write today's gap analysis
  scripts/gap_analysis.py --audit-help # list mmCore symbols missing HELP
  scripts/gap_analysis.py --date 2026-09-16
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE_C = os.path.join(REPO, "mmbasic", "src", "core.c")
EXPR_C = os.path.join(REPO, "mmbasic", "src", "expr.c")
ALL_COMMANDS = os.path.join(REPO, "picomite-fork", "AllCommands.h")
QB_KEYWORDS = os.path.join(REPO, "scripts", "data", "quickbasic_keywords.txt")
HELP_DIR = os.path.join(REPO, "docs", "help")

# Category order: common usage first; graphics later.
CATEGORIES = [
    "Control Flow",
    "Variables and Types",
    "Procedures",
    "Console and Text I/O",
    "String Operations",
    "Math Functions",
    "File System",
    "File I/O",
    "Time and Events",
    "System and Options",
    "Network and Terminal",
    "Sound and Play",
    "Graphics",
    "Sprites and Blit",
    "JSON and Structures",
    "Environment (MM.*)",
    "Hardware and Devices",
    "QuickBasic Legacy",
]

# Canonical name -> (category, 5-word description)
META: dict[str, tuple[str, str]] = {}


def _m(cat: str, desc: str, *names: str) -> None:
    for n in names:
        META[n.upper()] = (cat, desc)


def _load_meta() -> None:
    if META:
        return
    # Control Flow
    _m("Control Flow", "Comments out remaining program line", "REM")
    _m("Control Flow", "Runs counted loop with STEP", "FOR", "NEXT")
    _m("Control Flow", "Loops while condition remains true", "WHILE", "WEND")
    _m("Control Flow", "Provides general DO LOOP form", "DO", "LOOP")
    _m("Control Flow", "Branches with IF THEN blocks", "IF", "THEN", "ELSE", "ELSEIF", "ENDIF", "END IF", "ELSE IF")
    _m("Control Flow", "Selects among CASE branch paths", "SELECT", "CASE", "SELECT CASE", "CASE ELSE", "END SELECT")
    _m("Control Flow", "Jumps execution to named label", "GOTO")
    _m("Control Flow", "Calls subroutine at named label", "GOSUB", "RETURN")
    _m("Control Flow", "Leaves current loop or routine", "EXIT", "EXIT FOR", "EXIT DO", "EXIT SUB", "EXIT FUNCTION")
    _m("Control Flow", "Skips ahead to next iteration", "CONTINUE", "CONTINUE FOR", "CONTINUE DO")
    _m("Control Flow", "Ends the running program now", "END", "STOP", "SYSTEM")
    _m("Control Flow", "Handles ON events and jumps", "ON", "ON GOTO", "ON GOSUB", "ON ERROR", "ON KEY")
    _m("Control Flow", "Resumes after trapped runtime error", "RESUME")
    _m("Control Flow", "Chains into another program file", "CHAIN")
    _m("Control Flow", "Turns execution tracing on off", "TRON", "TROFF", "TRACE")

    # Variables and Types
    _m("Variables and Types", "Declares variables and array storage", "DIM", "REDIM")
    _m("Variables and Types", "Declares procedure local variable names", "LOCAL")
    _m("Variables and Types", "Declares static local variable storage", "STATIC")
    _m("Variables and Types", "Declares named compile time constants", "CONST")
    _m("Variables and Types", "Defines a user structured type", "TYPE", "END TYPE")
    _m("Variables and Types", "Assigns expression result to variable", "LET")
    _m("Variables and Types", "Embeds DATA values inside program", "DATA", "READ", "RESTORE")
    _m("Variables and Types", "Deletes variables and array storage", "ERASE", "CLEAR")
    _m("Variables and Types", "Swaps values of two variables", "SWAP")
    _m("Variables and Types", "Shares variables across procedure scopes", "SHARED", "COMMON")
    _m("Variables and Types", "Sets default lowest array index", "OPTION BASE")
    _m("Variables and Types", "Sets default type by letter", "DEFTYPE", "DEFSNG", "DEFDBL", "DEFINT", "DEFLNG", "DEFSTR")
    _m("Variables and Types", "Returns lower or upper bound", "LBOUND", "UBOUND", "BOUND")
    _m("Variables and Types", "Increments a numeric variable value", "INC")
    _m("Variables and Types", "Decrements a numeric variable value", "DEC")

    # Procedures
    _m("Procedures", "Defines a callable SUB routine", "SUB", "END SUB")
    _m("Procedures", "Defines a callable FUNCTION routine", "FUNCTION", "END FUNCTION")
    _m("Procedures", "Invokes a named SUB routine", "CALL")
    _m("Procedures", "Declares procedure name in advance", "DECLARE")
    _m("Procedures", "Runs a program or procedure", "RUN")
    _m("Procedures", "Defines a single line function", "DEF FN", "FN")
    _m("Procedures", "Runs an external shell command", "SHELL")

    # Console and Text I/O
    _m("Console and Text I/O", "Writes text to the console", "PRINT", "PRINT USING", "?")
    _m("Console and Text I/O", "Reads values from the console", "INPUT", "LINE INPUT")
    _m("Console and Text I/O", "Moves the console text cursor", "LOCATE")
    _m("Console and Text I/O", "Clears the display graphics screen", "CLS")
    _m("Console and Text I/O", "Reads keyboard without waiting pause", "INKEY$")
    _m("Console and Text I/O", "Tests whether keyboard key down", "KEYDOWN")
    _m("Console and Text I/O", "Returns current text cursor column", "POS", "CSRLIN", "LPOS")
    _m("Console and Text I/O", "Prints spaces or tab columns", "SPC", "TAB", "SPACE$")
    _m("Console and Text I/O", "Sets console output text width", "WIDTH")
    _m("Console and Text I/O", "Waits for a timed delay", "WAIT", "PAUSE", "SLEEP", "VSYNC_WAIT")
    _m("Console and Text I/O", "Writes text to printer stream", "LPRINT", "LPRINT USING")
    _m("Console and Text I/O", "Reads or writes hardware ports", "INP", "OUT")
    _m("Console and Text I/O", "Assigns strings to keyboard macros", "KEY")
    _m("Console and Text I/O", "Sounds a short speaker beep", "BEEP")
    _m("Console and Text I/O", "Sets printable text viewport window", "VIEW PRINT", "VIEW")

    # String Operations
    _m("String Operations", "Returns string length in characters", "LEN")
    _m("String Operations", "Returns ASCII code from character", "ASC")
    _m("String Operations", "Builds character from ASCII code", "CHR$")
    _m("String Operations", "Converts number into decimal string", "STR$")
    _m("String Operations", "Converts string into numeric value", "VAL")
    _m("String Operations", "Returns leftmost characters from string", "LEFT$")
    _m("String Operations", "Returns rightmost characters from string", "RIGHT$")
    _m("String Operations", "Extracts or replaces string substring", "MID$")
    _m("String Operations", "Converts string characters to uppercase", "UCASE$")
    _m("String Operations", "Converts string characters to lowercase", "LCASE$")
    _m("String Operations", "Finds starting position of substring", "INSTR")
    _m("String Operations", "Builds string of repeated characters", "STRING$")
    _m("String Operations", "Formats number as hexadecimal string", "HEX$")
    _m("String Operations", "Formats number as octal string", "OCT$")
    _m("String Operations", "Formats number as binary string", "BIN$")
    _m("String Operations", "Formats number into custom string", "FORMAT$")
    _m("String Operations", "Removes spaces from string edges", "LTRIM$", "RTRIM$")
    _m("String Operations", "Left or right justifies string", "LSET", "RSET")
    _m("String Operations", "Appends text onto string variable", "CAT")
    _m("String Operations", "Sorts values stored in array", "SORT")
    _m("String Operations", "Performs long string buffer operations", "LONGSTRING")
    _m("String Operations", "Defines fielded string inside record", "FIELD")
    _m("String Operations", "Converts values between numeric bases", "BASE$")
    _m("String Operations", "Converts binary buffer into string", "BIN2STR$")
    _m("String Operations", "Formats combined date time string", "DATETIME$")
    _m("String Operations", "Returns weekday name as string", "DAY$")
    _m("String Operations", "Returns directory listing as string", "DIR$")
    _m("String Operations", "Extracts delimited field from string", "FIELD$")
    _m("String Operations", "Gets substring from long string", "LGETSTR$")
    _m("String Operations", "Searches and replaces string text", "SCHANGE$")
    _m("String Operations", "Trims spaces from both sides", "TRIM$")

    # Math Functions
    _m("Math Functions", "Returns absolute value of number", "ABS")
    _m("Math Functions", "Truncates number value toward zero", "FIX", "INT", "CINT", "CLNG", "CDBL", "CSNG")
    _m("Math Functions", "Returns square root of number", "SQR", "SQRT")
    _m("Math Functions", "Returns sine of angle value", "SIN", "SINH")
    _m("Math Functions", "Returns cosine of angle value", "COS", "COSH")
    _m("Math Functions", "Returns tangent of angle value", "TAN", "TANH")
    _m("Math Functions", "Returns arctangent of numeric value", "ATN", "ATAN", "ATN2", "ATAN2")
    _m("Math Functions", "Returns inverse sine or cosine", "ASIN", "ASN", "ACOS", "ACS")
    _m("Math Functions", "Raises e to given power", "EXP")
    _m("Math Functions", "Returns natural logarithm of number", "LOG", "LOG10")
    _m("Math Functions", "Returns algebraic sign of number", "SGN")
    _m("Math Functions", "Returns or seeds random numbers", "RND", "RANDOMIZE")
    _m("Math Functions", "Returns mathematical constant value pi", "PI")
    _m("Math Functions", "Converts between degrees and radians", "DEG", "RAD")
    _m("Math Functions", "Returns maximum among given arguments", "MAX")
    _m("Math Functions", "Returns minimum among given arguments", "MIN")
    _m("Math Functions", "Chooses value based on condition", "CHOICE")
    _m("Math Functions", "Evaluates string text as expression", "EVAL")
    _m("Math Functions", "Performs array and matrix math", "MATH")
    _m("Math Functions", "Computes integer modulo remainder value", "MOD")
    _m("Math Functions", "Converts packed numeric binary forms", "CVI", "CVS", "CVD", "MKI$", "MKS$", "MKD$", "MKSMBF$", "MKDMBF$", "CVSMBF", "CVDMBF", "CVN", "MKN$")

    # File System
    _m("File System", "Changes the current working directory", "CHDIR")
    _m("File System", "Creates a new filesystem directory", "MKDIR")
    _m("File System", "Removes an empty filesystem directory", "RMDIR")
    _m("File System", "Deletes a file from storage", "KILL", "RM", "DEL")
    _m("File System", "Renames or moves a file", "RENAME", "NAME", "MV")
    _m("File System", "Copies a file between paths", "COPY", "XFER")
    _m("File System", "Lists files inside a directory", "DIR", "LS")
    _m("File System", "Opens fullscreen interactive file browser", "FILES")
    _m("File System", "Selects the active drive letter", "DRIVE")
    _m("File System", "Returns current working directory path", "CWD$")
    _m("File System", "Installs or manages software packages", "PACKAGE")
    _m("File System", "Loads or saves library modules", "LIBRARY")
    _m("File System", "Manages on device flash storage", "FLASH")
    _m("File System", "Transfers files using XMODEM protocol", "XMODEM", "YMODEM")

    # File I/O
    _m("File I/O", "Opens a file or device", "OPEN", "OPEN COM")
    _m("File I/O", "Closes an open file number", "CLOSE")
    _m("File I/O", "Seeks to given file position", "SEEK")
    _m("File I/O", "Tests whether file reached EOF", "EOF")
    _m("File I/O", "Returns length of open file", "LOF")
    _m("File I/O", "Returns current open file position", "LOC")
    _m("File I/O", "Reads characters from open file", "INPUT$")
    _m("File I/O", "Gets binary data from file", "GET")
    _m("File I/O", "Puts binary data into file", "PUT")
    _m("File I/O", "Writes values into open file", "WRITE")
    _m("File I/O", "Flushes pending buffered file output", "FLUSH")
    _m("File I/O", "Locks or unlocks file records", "LOCK", "UNLOCK")
    _m("File I/O", "Returns next unused file number", "FREEFILE")
    _m("File I/O", "Returns attributes for open file", "FILEATTR")
    _m("File I/O", "Resets and closes open files", "RESET")
    _m("File I/O", "Loads program text from storage", "LOAD")
    _m("File I/O", "Saves program text to storage", "SAVE")
    _m("File I/O", "Sends IOCTL device control strings", "IOCTL", "IOCTL$")

    # Time and Events
    _m("Time and Events", "Returns current calendar date string", "DATE$")
    _m("Time and Events", "Returns current clock time string", "TIME$")
    _m("Time and Events", "Reads millisecond resolution system timer", "TIMER")
    _m("Time and Events", "Configures periodic SETTICK interrupt timer", "SETTICK")
    _m("Time and Events", "Controls hardware watchdog timer behavior", "WATCHDOG")
    _m("Time and Events", "Returns from active interrupt handler", "IRETURN")
    _m("Time and Events", "Reports error number and line", "ERR", "ERL", "ERROR")
    _m("Time and Events", "Reports low level device errors", "ERDEV", "ERDEV$")
    _m("Time and Events", "Reads light pen input state", "PEN", "ON PEN")
    _m("Time and Events", "Reads joystick and button state", "STRIG", "ON STRIG", "STICK")
    _m("Time and Events", "Traps serial communications event signals", "ON COM", "COM")
    _m("Time and Events", "Traps periodic timer event signals", "ON TIMER")
    _m("Time and Events", "Traps background PLAY event signals", "ON PLAY")
    _m("Time and Events", "Traps programmable KEY event signals", "ON KEY")

    # System and Options
    _m("System and Options", "Clears loaded program from memory", "NEW")
    _m("System and Options", "Lists stored program source lines", "LIST")
    _m("System and Options", "Opens fullscreen program source editor", "EDIT", "EDIT FILE")
    _m("System and Options", "Opens simple fullscreen text editor", "WORDPAD")
    _m("System and Options", "Shows current memory usage statistics", "MEMORY")
    _m("System and Options", "Opens interactive on device help", "HELP", "IHELP")
    _m("System and Options", "Shows copyright and credit notices", "CREDITS")
    _m("System and Options", "Sets persistent interpreter option values", "OPTION", "OPTIONS")
    _m("System and Options", "Restores factory default option values", "FACTORY_RESET", "FACTORY RESET", "FACTORY")
    _m("System and Options", "Reboots the host hardware machine", "REBOOT", "RESTART", "CPU")
    _m("System and Options", "Closes the host MMBasic application", "QUIT")
    _m("System and Options", "Selects HDMI or jack audio", "AUDIO_TARGET", "AUDIO")
    _m("System and Options", "Configures immediate mode prompt format", "PROMPT")
    _m("System and Options", "Enables or disables Ethernet networking", "ETHERNET")
    _m("System and Options", "Configures and joins Wi-Fi networks", "WIFI")
    _m("System and Options", "Reads raw bytes from memory", "PEEK")
    _m("System and Options", "Writes raw bytes into memory", "POKE")
    _m("System and Options", "Returns amount of free memory", "FRE")
    _m("System and Options", "Defines current PEEK POKE segment", "DEF SEG")
    _m("System and Options", "Returns pointers into variable storage", "VARPTR", "VARSEG", "VARPTR$", "SADD")
    _m("System and Options", "Reads or writes environment variables", "ENVIRON", "ENVIRON$")
    _m("System and Options", "Autosaves current program edit buffer", "AUTOSAVE")
    _m("System and Options", "Executes string contents as code", "EXECUTE")
    _m("System and Options", "Updates installed device firmware image", "UPDATE FIRMWARE")
    _m("System and Options", "Defines embedded native C subroutine", "CSUB", "END CSUB", "DEFINEFONT", "END DEFINEFONT")
    _m("System and Options", "Saves or restores variable workspace", "VAR")
    _m("System and Options", "Controls optional source metacommand modes", "$STATIC", "$DYNAMIC")

    # Network and Terminal
    _m("Network and Terminal", "Helps establish network link connection", "CONNECT")
    _m("Network and Terminal", "Opens serial or TCP terminal", "TERM")
    _m("Network and Terminal", "Shows active IP network configuration", "IPCONFIG")
    _m("Network and Terminal", "Runs built-in web client utilities", "WEB")

    # Sound and Play
    _m("Sound and Play", "Plays tones or media files", "PLAY")
    _m("Sound and Play", "Reports whether audio currently playing", "PLAYING")
    _m("Sound and Play", "Plays simple PC speaker sounds", "SOUND")

    # Graphics
    _m("Graphics", "Sets or reads graphics pixel", "PIXEL")
    _m("Graphics", "Draws a straight graphics line", "LINE")
    _m("Graphics", "Draws filled or outline box", "BOX")
    _m("Graphics", "Draws circle outline or fill", "CIRCLE")
    _m("Graphics", "Draws rectangle with rounded corners", "RBOX")
    _m("Graphics", "Draws arc between two angles", "ARC")
    _m("Graphics", "Draws filled three point triangle", "TRIANGLE")
    _m("Graphics", "Draws multi point polygon shape", "POLYGON")
    _m("Graphics", "Draws text on graphics page", "TEXT")
    _m("Graphics", "Selects active graphics text font", "FONT")
    _m("Graphics", "Sets foreground and background colours", "COLOUR", "COLOR")
    _m("Graphics", "Selects active screen video mode", "MODE", "SCREEN", "RESOLUTION")
    _m("Graphics", "Selects active graphics drawing page", "PAGE")
    _m("Graphics", "Controls offscreen framebuffer page usage", "FRAMEBUFFER")
    _m("Graphics", "Loads and transforms image bitmaps", "IMAGE")
    _m("Graphics", "Runs LOGO style turtle graphics", "TURTLE")
    _m("Graphics", "Plots packed bitmap pattern image", "BITMAP", "GUI BITMAP", "GUI")
    _m("Graphics", "Builds packed RGB colour value", "RGB")
    _m("Graphics", "Fills connected graphics region area", "PAINT", "FILL")
    _m("Graphics", "Maps indexed palette colour values", "PALETTE", "MAP", "COLOUR MAP")
    _m("Graphics", "Copies pixels between screen pages", "PCOPY")
    _m("Graphics", "Maps coordinates between graphics spaces", "PMAP", "WINDOW", "POINT")
    _m("Graphics", "Sets or clears graphics point", "PRESET", "PSET")
    _m("Graphics", "Runs DRAW graphics macro language", "DRAW")
    _m("Graphics", "Draws Bezier curve graphics paths", "BEZIER")
    _m("Graphics", "Controls tiled map display layers", "TILE", "TILEMAP")
    _m("Graphics", "Provides helpers for 3D drawing", "DRAW3D")
    _m("Graphics", "Captures frames from camera device", "CAMERA")
    _m("Graphics", "Renders Mandelbrot fractal image output", "MANDELBROT")
    _m("Graphics", "Runs realtime raycaster render engine", "RAY")
    _m("Graphics", "Draws animated starfield graphics effect", "STAR", "ASTRO")
    _m("Graphics", "Refreshes or adjusts display output", "REFRESH", "BACKLIGHT")

    # Sprites and Blit
    _m("Sprites and Blit", "Loads shows moves sprite objects", "SPRITE")
    _m("Sprites and Blit", "Copies rectangular bit-block images", "BLIT", "BLIT MEMORY")

    # JSON and Structures
    _m("JSON and Structures", "Operates on structured record arrays", "STRUCT")
    _m("JSON and Structures", "Parses JSON text into variables", "JSON_PARSE")
    _m("JSON and Structures", "Reads JSON path as string", "JSON$")
    _m("JSON and Structures", "Serializes value into JSON text", "JSON_STRINGIFY$")
    _m("JSON and Structures", "Slices inserts or fills arrays", "ARRAY SLICE", "ARRAY INSERT", "ARRAY ADD", "ARRAY SET")

    # Environment
    _m("Environment (MM.*)", "Returns horizontal resolution in pixels", "MM.HRES")
    _m("Environment (MM.*)", "Returns vertical resolution in pixels", "MM.VRES")
    _m("Environment (MM.*)", "Returns text cursor column position", "MM.HPOS")
    _m("Environment (MM.*)", "Returns text cursor row position", "MM.VPOS")
    _m("Environment (MM.*)", "Queries MMBasic system info values", "MM.INFO", "MM.INFO$")
    _m("Environment (MM.*)", "Returns firmware version string value", "MM.VER")
    _m("Environment (MM.*)", "Returns current device name string", "MM.DEVICE$")
    _m("Environment (MM.*)", "Returns startup command line string", "MM.CMDLINE$")

    # Hardware
    _m("Hardware and Devices", "Configures mode for GPIO pin", "SETPIN", "PIN(")
    _m("Hardware and Devices", "Accesses multi bit digital ports", "PORT(")
    _m("Hardware and Devices", "Pulses selected GPIO pin output", "PULSE", "PULSIN")
    _m("Hardware and Devices", "Controls hardware PWM output channels", "PWM")
    _m("Hardware and Devices", "Communicates over I2C device bus", "I2C", "I2C2", "I2CLCD")
    _m("Hardware and Devices", "Communicates over SPI device bus", "SPI", "SPI2")
    _m("Hardware and Devices", "Communicates over one wire bus", "ONEWIRE", "TEMPR START", "TEMPR")
    _m("Hardware and Devices", "Controls onboard real time clock", "RTC")
    _m("Hardware and Devices", "Reads analog to digital converter", "ADC")
    _m("Hardware and Devices", "Handles infrared remote control codes", "IR")
    _m("Hardware and Devices", "Drives WS2812 addressable LED strips", "WS2812")
    _m("Hardware and Devices", "Controls positions of servo motors", "SERVO")
    _m("Hardware and Devices", "Controls motion of stepper motors", "STEPPER", "SLEW", "TMC22XX")
    _m("Hardware and Devices", "Programs PIO hardware state machines", "PIO")
    _m("Hardware and Devices", "Runs peripheral device helper commands", "DEVICE")
    _m("Hardware and Devices", "Reads state from mouse device", "MOUSE")
    _m("Hardware and Devices", "Reads state from gamepad device", "GAMEPAD")
    _m("Hardware and Devices", "Talks with Wii controller hardware", "WII", "WII CLASSIC", "WII NUNCHUCK")
    _m("Hardware and Devices", "Scans keys on matrix keypad", "KEYPAD")
    _m("Hardware and Devices", "Reads humidity from sensor device", "HUMID")
    _m("Hardware and Devices", "Drives attached LCD panel displays", "LCD")
    _m("Hardware and Devices", "Generates timed digital bitstream output", "BITSTREAM", "ONESHOT")
    _m("Hardware and Devices", "Helps synchronize display timing signals", "SYNC")
    _m("Hardware and Devices", "Accesses external PSRAM memory helpers", "RAM")
    _m("Hardware and Devices", "Controls FM radio peripheral helpers", "FM")
    _m("Hardware and Devices", "Configures attached keyboard device options", "KEYBOARD")
    _m("Hardware and Devices", "Configures selected hardware peripheral block", "CONFIGURE")
    _m("Hardware and Devices", "Reads values from distance sensor", "DISTANCE")
    _m("Hardware and Devices", "Reads values from GPS helper", "GPS")
    _m("Hardware and Devices", "Reads values from touch panel", "TOUCH")
    _m("Hardware and Devices", "Drives interactive on-screen GUI controls", "CTRLVAL", "MSGBOX", "CLICK")
    _m("Hardware and Devices", "Manipulates byte flag bit fields", "BIT(", "BYTE(", "FLAG(", "FLAGS")
    _m("Hardware and Devices", "Hooks interrupt to CSUB handler", "INTERRUPT")
    _m("Hardware and Devices", "Loads and runs CMM2 programs", "CMM2 LOAD", "CMM2 RUN")
    _m("Hardware and Devices", "Provides helpers for frame timing", "FRAME", "LOCATION")
    _m("Hardware and Devices", "Enables interactive calculator prompt mode", "CALC")

    # QuickBasic Legacy leftovers
    _m("QuickBasic Legacy", "Loads or saves memory images", "BLOAD", "BSAVE")
    _m("QuickBasic Legacy", "Performs absolute far memory CALL", "CALL ABSOLUTE")
    _m("QuickBasic Legacy", "Provides boolean logical operator keywords", "AND", "OR", "XOR", "NOT", "EQV", "IMP")
    _m("QuickBasic Legacy", "Supports counted loop step keywords", "STEP", "TO", "AS")
    _m("QuickBasic Legacy", "Provides optional event OFF keyword", "OFF")


def canon(name: str) -> str:
    n = name.strip()
    n = n.replace("(", "").replace(")", "")
    n = re.sub(r"\s+", " ", n)
    return n.upper()


def extract_mmcore_commands() -> set[str]:
    text = open(CORE_C, encoding="utf-8", errors="replace").read()
    # Limit to try_tok_cmd body roughly
    m = re.search(r"static int try_tok_cmd\(void\)\s*\{(.*?)^\}", text, re.S | re.M)
    body = m.group(1) if m else text
    names = {canon(x) for x in re.findall(r'tab\[mmb_kw_id\("([^"]+)"\)\]', body)}
    core_all = open(CORE_C, encoding="utf-8", errors="replace").read()
    # Compound forms implemented via wrappers / matchers in core.c
    compound_markers = {
        "LINE INPUT": r"LINE\s+INPUT|mmb_match\(\"INPUT\"\)|tok_cmd_line",
        "END IF": r"END\s+IF|mmb_match\(\"IF\"\)",
        "END SELECT": r"END\s+SELECT|mmb_match\(\"SELECT\"\)",
        "END SUB": r"END\s+SUB|mmb_match\(\"SUB\"\)",
        "END FUNCTION": r"END\s+FUNCTION|mmb_match\(\"FUNCTION\"\)",
        "END TYPE": r"END\s+TYPE|mmb_match\(\"TYPE\"\)",
        "FACTORY RESET": r"FACTORY\s+RESET|tok_cmd_factory",
        "GUI BITMAP": r"GUI\s+BITMAP|mmb_match\(\"BITMAP\"\)",
        "LIST FILES": r"LIST\s+FILES|mmb_match\(\"FILES\"\)",
        "LIST TYPE": r"LIST\s+TYPE|mmb_match\(\"TYPE\"\)",
        "SELECT CASE": r"SELECT\s+CASE|mmb_cmd_select",
        "EXIT FOR": r"EXIT\s+FOR|mmb_match\(\"FOR\"\)",
        "EXIT DO": r"EXIT\s+DO|mmb_match\(\"DO\"\)",
        "EXIT SUB": r"EXIT\s+SUB|mmb_match\(\"SUB\"\)",
        "EXIT FUNCTION": r"EXIT\s+FUNCTION|mmb_match\(\"FUNCTION\"\)",
        "CONTINUE FOR": r"CONTINUE\s+FOR|mmb_match\(\"FOR\"\)",
        "CONTINUE DO": r"CONTINUE\s+DO|mmb_match\(\"DO\"\)",
        "ON GOTO": r"ON\s+GOTO|mmb_match\(\"GOTO\"\)",
        "ON GOSUB": r"ON\s+GOSUB|mmb_match\(\"GOSUB\"\)",
        "ON KEY": r"ON\s+KEY|mmb_match\(\"KEY\"\)",
        "ON ERROR": r"ON\s+ERROR|mmb_match\(\"ERROR\"\)",
    }
    for name, pat in compound_markers.items():
        if name.split()[0] in names or re.search(pat, core_all, re.I):
            names.add(name)
    return names


def extract_mmcore_functions() -> set[str]:
    text = open(EXPR_C, encoding="utf-8", errors="replace").read()
    m = re.search(r"int mmb_try_function\(mmb_val \*out\)\s*\{(.*?)^\}", text, re.S | re.M)
    body = m.group(1) if m else text
    names = {canon(x) for x in re.findall(r'fun_tab\[mmb_kw_id\("([^"]+)"\)\]', body)}
    names.add("STRUCT")  # STRUCT(...) function family also
    return names


def extract_picomite() -> tuple[set[str], set[str]]:
    text = open(ALL_COMMANDS, encoding="utf-8", errors="replace").read()
    cmds: set[str] = set()
    funs: set[str] = set()
    cmd_sec = re.search(
        r"#ifdef INCLUDE_COMMAND_TABLE(.*?)#endif /\* INCLUDE_COMMAND_TABLE \*/",
        text,
        re.S,
    )
    tok_sec = re.search(
        r"#ifdef INCLUDE_TOKEN_TABLE(.*?)#endif /\* INCLUDE_TOKEN_TABLE \*/",
        text,
        re.S,
    )
    row_re = re.compile(
        r'\{\s*\(unsigned char \*\)"([^"]+)"\s*,\s*([^,]+),',
        re.M,
    )
    if cmd_sec:
        for name, flags in row_re.findall(cmd_sec.group(1)):
            if name.startswith("_"):
                continue  # PIO assembler noise
            c = canon(name)
            if not c or c in {"/*", "*/"}:
                continue
            cmds.add(c)
            if "T_FUN" in flags:
                funs.add(c)
    if tok_sec:
        for name, flags in row_re.findall(tok_sec.group(1)):
            c = canon(name)
            if not c:
                continue
            if "T_FUN" in flags or "T_FNA" in flags:
                funs.add(c)
            # skip pure operators
            if flags.strip().startswith("T_OPER"):
                continue
    return cmds, funs


def extract_quickbasic() -> set[str]:
    names: set[str] = set()
    if os.path.isfile(QB_KEYWORDS):
        for line in open(QB_KEYWORDS, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#"):
                names.add(canon(line))
    return names


def classify(name: str) -> tuple[str, str]:
    _load_meta()
    if name in META:
        return META[name]
    # Heuristics for leftovers
    if name.startswith("MM."):
        return "Environment (MM.*)", "Queries MMBasic environment info value"
    if name.startswith("ON "):
        return "Control Flow", "Handles traps or computed jumps"
    if name.startswith("END ") or name.startswith("EXIT "):
        return "Control Flow", "Ends or exits structured block"
    if name.endswith("$"):
        return "String Operations", "Provides string valued language function"
    if any(name.startswith(p) for p in ("I2C", "SPI", "PIO", "PWM", "ADC", "RTC")):
        return "Hardware and Devices", "Provides hardware device command family"
    return "System and Options", "Legacy language or system keyword"


def five_words(desc: str) -> str:
    """Return exactly five words for the gap-analysis table."""
    words = [w for w in desc.split() if w]
    if len(words) == 5:
        return " ".join(words)
    if len(words) > 5:
        return " ".join(words[:5])
    # Last-resort expansion for unclassified leftovers
    while len(words) < 5:
        words.append("keyword")
    return " ".join(words)


def mark(flag: bool) -> str:
    return "x" if flag else ""


CATCH_ALL = {"FUNCTIONS", "MATH"}
COVERAGE = os.path.join(REPO, "scripts", "data", "help_coverage.tsv")


def load_coverage(path: str = COVERAGE) -> dict[str, tuple[str, str, str]]:
    """Read the checked-in symbol -> topic-or-alias coverage list."""
    cov: dict[str, tuple[str, str, str]] = {}
    with open(path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 4:
                raise SystemExit(f"{path}:{lineno}: expected 4 tab fields: {line!r}")
            sym, target, kind, note = (p.strip() for p in parts[:4])
            if not sym:
                continue
            key = sym.upper()
            if key in cov:
                raise SystemExit(f"{path}:{lineno}: duplicate symbol {sym}")
            if kind not in ("topic", "alias"):
                raise SystemExit(f"{path}:{lineno}: kind must be topic or alias")
            cov[key] = (target.upper(), kind, note)
    return cov


def audit_help(mm_cmds: set[str], mm_funs: set[str]) -> int:
    """Fail when an implemented symbol has no HELP topic and no justified alias.

    Every symbol must appear in scripts/data/help_coverage.tsv. A symbol marked
    ``alias`` may not be covered only by a catch-all catalogue page such as
    FUNCTIONS or MATH: it needs a topic, or an alias to a real command page.
    """
    sys.path.insert(0, os.path.join(REPO, "scripts"))
    import gen_help  # noqa: WPS433

    topics = gen_help.load_topics(HELP_DIR)
    names = {t["name"].upper() for t in topics}
    cov = load_coverage()
    problems = []
    for name in sorted(mm_cmds | mm_funs):
        entry = cov.get(name)
        if entry is None:
            problems.append(f"{name}: no entry in help_coverage.tsv")
            continue
        target, kind, note = entry
        if target not in names:
            problems.append(f"{name}: target {target} is not a help topic")
            continue
        if kind == "topic" and target != name:
            problems.append(f"{name}: marked topic but targets {target}")
        if kind == "alias" and target in CATCH_ALL and "catch-all" not in note.lower():
            problems.append(
                f"{name}: alias-only to catch-all {target} (needs its own topic)"
            )
    stale = sorted(s for s in cov if s not in (mm_cmds | mm_funs))
    print(f"mmCore symbols missing/incorrect HELP coverage: {len(problems)}")
    for p in problems:
        print(f"  {p}")
    if stale:
        print(f"note: {len(stale)} stale coverage entries (no longer implemented):")
        for s in stale:
            print(f"  {s}")
    return 0 if not problems else 1


def write_report(
    path: str,
    mm_cmds: set[str],
    mm_funs: set[str],
    pico_cmds: set[str],
    pico_funs: set[str],
    qb: set[str],
) -> None:
    mm = {canon(x) for x in (mm_cmds | mm_funs)}
    pico = {canon(x) for x in (pico_cmds | pico_funs)}
    qb = {canon(x) for x in qb}

    # Alias folding for presence checks (COLOR/COLOUR etc.)
    def present(name: str, universe: set[str]) -> bool:
        if name in universe:
            return True
        alts = {
            "COLOUR": ["COLOR"],
            "COLOR": ["COLOUR"],
            "SQR": ["SQRT"],
            "SQRT": ["SQR"],
            "ATN": ["ATAN", "ATN2", "ATAN2"],
            "ATAN": ["ATN"],
            "ACOS": ["ACS"],
            "ACS": ["ACOS"],
            "ASIN": ["ASN"],
            "ASN": ["ASIN"],
            "KILL": ["RM", "DEL"],
            "RENAME": ["NAME", "MV"],
            "DIR": ["LS"],
            "REBOOT": ["RESTART"],
            "HELP": ["IHELP"],
            "FACTORY_RESET": ["FACTORY RESET", "FACTORY"],
            "SELECT CASE": ["SELECT"],
            "ENDIF": ["END IF"],
            "END IF": ["ENDIF"],
            "ELSEIF": ["ELSE IF"],
            "ELSE IF": ["ELSEIF"],
            "ON ERROR": ["ON"],
            "ON KEY": ["ON"],
            "ON GOTO": ["ON"],
            "ON GOSUB": ["ON"],
            "ON TIMER": ["ON", "TIMER"],
            "ON PLAY": ["ON", "PLAY"],
            "ON PEN": ["ON", "PEN"],
            "ON STRIG": ["ON", "STRIG"],
            "ON COM": ["ON", "COM"],
        }.get(name, [])
        if any(a in universe for a in alts):
            return True
        # PicoMite often implements compound forms under the parent token
        if " " in name:
            parent = name.split()[0]
            if parent in universe and parent in {"ON", "END", "EXIT", "CONTINUE", "SELECT", "CASE", "ELSE", "LINE", "ARRAY", "BLIT", "FACTORY", "GUI", "UPDATE", "TEMPR", "WII", "CMM2", "IRQ"}:
                return True
        return False

    all_names = set(mm) | set(pico) | set(qb)
    # Drop pure punctuation / tiny noise
    all_names = {n for n in all_names if n and n not in {"/*", "*/"} and not n.startswith("_")}

    by_cat: dict[str, list[str]] = defaultdict(list)
    for name in sorted(all_names, key=lambda s: (s.replace("$", ""), s)):
        cat, _ = classify(name)
        # If only in QB and category would be System, prefer QuickBasic Legacy for obscure ones
        if name in qb and name not in mm and name not in pico:
            cat2, _ = classify(name)
            if cat2 in {"System and Options", "Console and Text I/O"} and name in {
                "BLOAD",
                "BSAVE",
                "CALL ABSOLUTE",
                "CVI",
                "CVS",
                "CVD",
                "MKI$",
                "MKS$",
                "MKD$",
                "SADD",
                "VARPTR",
                "VARSEG",
                "VARPTR$",
                "IOCTL",
                "IOCTL$",
                "ERDEV",
                "ERDEV$",
                "DEF SEG",
                "DEF FN",
                "$STATIC",
                "$DYNAMIC",
                "TRON",
                "TROFF",
                "LSET",
                "RSET",
                "LTRIM$",
                "RTRIM$",
                "FIELD",
                "VIEW PRINT",
                "OPEN COM",
                "PRINT USING",
                "LPRINT USING",
            }:
                cat = "QuickBasic Legacy"
            elif name not in META and cat == "System and Options":
                cat = "QuickBasic Legacy"
        by_cat[cat].append(name)

    lines: list[str] = []
    lines.append("# Command and function gap analysis")
    lines.append("")
    lines.append(f"Generated: {os.path.basename(path).replace('GAP_ANALYSIS_', '').replace('.md', '')}")
    lines.append("")
    lines.append("Inventories:")
    lines.append("")
    lines.append("- **mmCore** — commands in `mmbasic/src/core.c` (`try_tok_cmd`) and functions in `mmbasic/src/expr.c` (`mmb_try_function`), plus noted compound forms.")
    lines.append("- **MMBasic** — PicoMite `picomite-fork/AllCommands.h` command and function token tables (reference submodule).")
    lines.append("- **QuickBasic** — keyword list derived from https://gamma.zem.fi/~fis/qb.html (see `scripts/data/quickbasic_keywords.txt`).")
    lines.append("")
    lines.append("An `x` means the symbol is available in that inventory. Subcommands may appear as separate rows. This file is Markdown documentation only and is **not** compiled into the HELP firmware image.")
    lines.append("")
    lines.append("Regenerate with `scripts/gap_analysis.py`. Do not open GitHub issues from this file unless explicitly asked.")
    lines.append("")
    lines.append("## Summary counts")
    lines.append("")
    lines.append("| Inventory | Symbols |")
    lines.append("|-----------|--------:|")
    lines.append(f"| mmCore | {len(mm)} |")
    lines.append(f"| MMBasic (PicoMite) | {len(pico)} |")
    lines.append(f"| QuickBasic | {len(qb)} |")
    lines.append(f"| Union | {len(all_names)} |")
    lines.append(f"| In PicoMite / QB but not mmCore | {len((pico | qb) - mm)} |")
    lines.append(f"| In mmCore only | {len(mm - pico - qb)} |")
    lines.append("")

    for cat in CATEGORIES:
        names = by_cat.get(cat, [])
        if not names:
            continue
        lines.append(f"## {cat}")
        lines.append("")
        lines.append("| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |")
        lines.append("|---|---|:---:|:---:|:---:|")
        for name in names:
            _, desc = classify(name)
            lines.append(
                f"| `{name}` | {five_words(desc)} | {mark(present(name, mm))} | {mark(present(name, pico))} | {mark(present(name, qb))} |"
            )
        lines.append("")

    # Any leftover categories
    for cat, names in sorted(by_cat.items()):
        if cat in CATEGORIES:
            continue
        lines.append(f"## {cat}")
        lines.append("")
        lines.append("| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |")
        lines.append("|---|---|:---:|:---:|:---:|")
        for name in names:
            _, desc = classify(name)
            lines.append(
                f"| `{name}` | {five_words(desc)} | {mark(present(name, mm))} | {mark(present(name, pico))} | {mark(present(name, qb))} |"
            )
        lines.append("")

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines).rstrip() + "\n")
    print(f"Wrote {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--date", help="YYYY-MM-DD for output filename (default: today)")
    ap.add_argument("--audit-help", action="store_true", help="List mmCore symbols missing HELP")
    ap.add_argument("-o", "--output", help="Override output path")
    args = ap.parse_args()

    mm_cmds = extract_mmcore_commands()
    mm_funs = extract_mmcore_functions()
    pico_cmds, pico_funs = extract_picomite()
    qb = extract_quickbasic()

    if args.audit_help:
        return audit_help(mm_cmds, mm_funs)

    day = args.date or dt.date.today().isoformat()
    out = args.output or os.path.join(HELP_DIR, f"GAP_ANALYSIS_{day}.md")
    write_report(out, mm_cmds, mm_funs, pico_cmds, pico_funs, qb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
