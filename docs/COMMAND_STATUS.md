# CMM2 command status (implementation gap map)

Status of the Colour Maximite 2 (CMM2) command/function surface against the
current codebase. The MMBasic interpreter core from `picomite-fork` is **not yet
ported onto Circle** — the `console/` app is only a placeholder REPL used to
exercise the QEMU harness. So almost the entire language is unimplemented; this
document is the porting backlog, organised by category.

Legend: **✗** not implemented · **◐** partial (limited demonstrator version) ·
**✓** complete/CMM2-equivalent (none yet).

Command/function names are drawn from the CMM2 User Manual reference (Commands
and Functions listings) and are representative rather than byte-exhaustive.

---

## Implemented today (all partial)

| Item | Status | What's missing vs CMM2 |
| --- | --- | --- |
| `PRINT` | ◐ | Only a single quoted string or one integer `a op b` (`+ - * /`). No variables, multiple items, `,`/`;` separators, `TAB`, floats, string expressions, or `PRINT #file`. |
| `CLS` | ◐ | Clears to a named colour; no `RGB()`/background-colour model. |
| `PIXEL` | ◐ | Sets a pixel in one of 8 named colours; no `RGB()`, no `PIXEL()` read function. |
| `LINE` | ◐ | `x1,y1,x2,y2[,colour]` only; no line-width, no `RGB()`. |
| `BOX` | ◐ | Outline only; no line-width, no fill colour. |
| `CIRCLE` | ◐ | Outline only; no line-width, aspect ratio, or fill. |

Everything below is **✗ not implemented**.

---

## 1. Program entry, editor, loader, directives — ✗
`'` (comment), `REM`, `?` (PRINT shortcut), `*` (run), `#COMMENT`, `#DEFINE`,
`#INCLUDE`, `#MMDEBUG` / `MMDEBUG`, `AUTOSAVE`, `EDIT`, `LIST`, `LOAD`, `SAVE`,
`RUN`, `NEW`, `TRACE`, `XMODEM`, `EXECUTE`.

## 2. Variables & data declaration — ✗
`DIM`, `CONST`, `LET`, `VAR` (SAVE/RESTORE), `LOCAL`, `STATIC`, `DATA`, `READ`,
`RESTORE`, `ERASE`, `CLEAR`, `LONGSTRING …`, `OPTION` (DEFAULT/BASE/EXPLICIT…).

## 3. Control flow — ✗
`IF` / `ELSE` / `ELSEIF` / `ENDIF`, `DO` / `LOOP`, `FOR` / `NEXT`,
`SELECT` / `CASE`, `GOTO`, `GOSUB` / `RETURN`, `ON … GOTO/GOSUB`, `EXIT`,
`CONTINUE`, `END`, `ON ERROR` / `ERROR`.

## 4. Subroutines & functions — ✗
`SUB` / `END SUB`, `FUNCTION` / `END FUNCTION`, `CALL`, `CSUB`, `EXIT SUB/FUNCTION`.

## 5. Operators & expression engine — ✗
Full expression evaluator: arithmetic (`+ - * / \ MOD ^`), relational, logical
(`AND OR NOT XOR`), string concatenation, operator precedence, floats + 64-bit
integers. (The placeholder only evaluates one binary integer op.)

## 6. Math functions — ✗
`ABS ACOS ASIN ATN ATAN2 COS SIN TAN EXP LOG SQR INT FIX CINT SGN RND DEG RAD
MIN MAX PI MATH() DISTANCE()`.

## 7. String functions — ✗
`ASC CHR$ LEFT$ RIGHT$ MID$ LEN INSTR STR$ VAL UCASE$ LCASE$ SPACE$ STRING$
FORMAT$ HEX$ OCT$ BIN$ BASE$ FIELD$ TAB BIN2STR$ STR2BIN EVAL JSON$ CHOICE
BOUND`; long-string ops `LGETBYTE LGETSTR$ LINSTR LLEN LCOMPARE`.

## 8. Console input — ✗
`INPUT`, `LINE INPUT`, `INKEY$()`, `KEYDOWN()`, `MM.*` console state.

## 9. Graphics — colour & text — ✗
`COLOUR`/`COLOR`, `RGB()`, `FONT`, `DEFINEFONT`, `TEXT`, `MODE` (video modes).

## 10. Graphics — 2D primitives — ◐ / ✗
◐ `PIXEL` `LINE` `BOX` `CIRCLE` (see above). ✗ `ARC`, `RBOX`, `TRIANGLE`,
`POLYGON`, `PIXEL()` (read), line-width/fill options on all primitives.

## 11. Graphics — images, sprites, pages, blit — ✗
`IMAGE`, `BLIT`, `SPRITE` (+ `SPRITE()`), `MAP` (+ `MAP()`), `PAGE`,
`FRAMEBUFFER`, `GUI` controls, `LOAD IMAGE` / `SAVE IMAGE`.

## 12. Graphics — advanced / 3D / turtle — ✗
`DRAW3D` (+ `DRAW3D()`), `TURTLE`, geometry helpers in `MATH`.

## 13. Sound & music — ✗
`PLAY` (tones/MOD/WAV/MIDI), `DAC`, PWM/analog sound output.

## 14. Files & SD card — ✗
`OPEN`, `CLOSE`, `PRINT #`, `INPUT #`, `LINE INPUT #`, `SEEK`, `FILES`, `LS`,
`KILL`, `COPY`, `RENAME`, `MKDIR`, `RMDIR`, `CHDIR`; functions `EOF() LOC() LOF()
DIR$() INC`.

## 15. Date/time & timers — ✗
`TIME$`/`DATE$`/`DATETIME$` (cmd+fn), `DAY$()`, `EPOCH()`, `PAUSE`, `TIMER`
(cmd+fn), `SETTICK`, `WATCHDOG`, `CPU` (clock/restart).

## 16. GPIO / analog / PWM / servo / measurement — ✗ (hardware; largely deferred)
`PIN` (cmd+fn), `SETPIN`, `PORT()`, `PULSE`, `PULSIN()`, `PWM`, `SERVO`, `ADC`,
`DAC`, `BITBANG`, `IR`, `TEMPR` (cmd+fn), `HUMID`, `GPS()`. On bare-metal Pi
these map to Pi peripherals rather than the CMM2 pinout.

## 17. Interrupts — ✗
Pin-change interrupts via `SETPIN`, `SETTICK`, `IR`, keyboard/serial interrupts.

## 18. Communications — ✗ (explicitly optional / deferred)
Async serial (`OPEN "COMx:"`, `BAUDRATE()`, Appendix A), `I2C`, `SPI`/`SPI2()`,
`ONEWIRE`.

## 19. Input devices — ✗ (Wii / controllers explicitly optional)
`MOUSE` (cmd+fn), `WII`, `CONTROLLER` (Nunchuk/Classic/gamepad), `IR`,
`KEYDOWN()`.

## 20. System / options / memory / misc — ✗
`OPTION`, `CPU`, `WATCHDOG`, `MEMORY`, `POKE` / `PEEK()`, `VAR SAVE/RESTORE`,
`CLASSIC()`, `EVAL()`, `SETTICK`, error handling state.

---

## Priorities

Per the project owner, **graphics equivalence is the priority** (categories
9–12), the core language + console (1–8) is the prerequisite, and communications
+ input-device integrations (18–19) are optional/deferred. See
[`ROADMAP.md`](ROADMAP.md).
