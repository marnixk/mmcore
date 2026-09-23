' TDF.BAS - TheDraw (.TDF) font loader and banner printer for MMBasic.
'
' TheDraw fonts are seeded at A:/fonts/tdf/ (see issue #554). Include this
' library, load a font, then draw strings in TEXT mode:
'
'   #INCLUDE "A:/lib/TDF.BAS"
'   TDF.Load "A:/fonts/tdf/STANDARD.TDF"
'   TDF.Print 0, 3, "HELLO"
'   TDF.Close
'
' TDF.Load also accepts a second form, TDF.LoadNamed path$, name$, to pick one
' font out of a multi-font collection.
'
' After a load: TDF.Name$ (font name), TDF.Type% (0 Outline, 1 Block, 2 Color),
' TDF.Spacing% and TDF.Height. TDF.Width(s$) returns the column advance for s$
' without drawing. TDF.Print x, y, s$ draws at character cell x (column) /
' y (row) and leaves the cursor after the last glyph.
'
' Glyph rows are read until the NUL terminator, so descender ('&') lines are
' kept even when the Max Height header under-counts them. Color fonts use a
' (character, attribute) byte pair; attribute 00 is a legal colour, only a NUL
' character ends the glyph. Outline fonts are remapped from A-O / @ / & to the
' CP437 box-drawing cells documented by Roy/SAC:
'   https://www.roysac.com/blog/2014/04/thedraw-fonts-file-tdf-specifications/
'
' While a font is loaded this library owns file stream #10; call TDF.Close to
' release it.

CONST TDF_FIRST% = 33
CONST TDF_LAST% = 126
CONST TDF_GLYPHS% = 94
CONST TDF_RECORD% = 213
CONST TDF_UNDEF% = 65535

DIM TDF.path$
DIM TDF.name$
DIM TDF.type%
DIM TDF.spacing%
DIM TDF.height
DIM TDF.data%
DIM TDF.open%
DIM TDF.fn%
DIM TDF.off%(93)
DIM TDF.wid%(93)
DIM TDF.hgt%(93)
DIM TDF.def%(93)

TDF.open% = 0
TDF.fn% = 10
TDF.name$ = ""
TDF.path$ = ""
TDF.spacing% = 1
TDF.height = 0

' Read one byte at the current file position. A NUL byte and EOF both read as
' an empty string, so both come back as 0; glyph parsing stops at NUL anyway.
FUNCTION TDF.NextByte()
  LOCAL c$
  c$ = INPUT$(1, TDF.fn%)
  IF LEN(c$) = 0 THEN
    TDF.NextByte = 0
  ELSE
    TDF.NextByte = ASC(c$)
  END IF
END FUNCTION

FUNCTION TDF.AtByte(pos%)
  SEEK #TDF.fn%, pos%
  TDF.AtByte = TDF.NextByte()
END FUNCTION

' Outline glyph letters to CP437 cells (see the Roy/SAC TDF spec table).
FUNCTION TDF.OutlineCode(c%)
  SELECT CASE c%
    CASE 65: TDF.OutlineCode = 205
    CASE 66: TDF.OutlineCode = 196
    CASE 67: TDF.OutlineCode = 179
    CASE 68: TDF.OutlineCode = 186
    CASE 69: TDF.OutlineCode = 213
    CASE 70: TDF.OutlineCode = 187
    CASE 71: TDF.OutlineCode = 214
    CASE 72: TDF.OutlineCode = 191
    CASE 73: TDF.OutlineCode = 200
    CASE 74: TDF.OutlineCode = 190
    CASE 75: TDF.OutlineCode = 192
    CASE 76: TDF.OutlineCode = 189
    CASE 77: TDF.OutlineCode = 181
    CASE 78: TDF.OutlineCode = 199
    CASE 79: TDF.OutlineCode = 247
    CASE 64: TDF.OutlineCode = 32
    CASE ELSE: TDF.OutlineCode = c%
  END SELECT
END FUNCTION

SUB TDF.Close
  IF TDF.open% THEN
    CLOSE #TDF.fn%
  END IF
  TDF.open% = 0
END SUB

' Open path$ and check the TheDraw file header before any font parsing.
SUB TDF.OpenFile(path$)
  LOCAL magic$, sz%
  IF TDF.open% THEN CALL TDF.Close
  OPEN path$ FOR INPUT AS #TDF.fn%
  TDF.open% = 1
  sz% = LOF(#TDF.fn%)
  IF sz% < TDF_RECORD% THEN
    CALL TDF.Close
    ERROR "TDF: file not found or too small"
  END IF
  SEEK #TDF.fn%, 0
  magic$ = INPUT$(20, TDF.fn%)
  IF LEN(magic$) <> 20 THEN
    CALL TDF.Close
    ERROR "TDF: not a TheDraw font"
  END IF
  IF ASC(MID$(magic$, 1, 1)) <> 19 THEN
    CALL TDF.Close
    ERROR "TDF: not a TheDraw font"
  END IF
  IF MID$(magic$, 20, 1) <> CHR$(26) THEN
    CALL TDF.Close
    ERROR "TDF: not a TheDraw font"
  END IF
  IF INSTR(magic$, "TheDraw FONTS file") <> 2 THEN
    CALL TDF.Close
    ERROR "TDF: not a TheDraw font"
  END IF
  IF TDF.AtByte(20) <> 85 OR TDF.AtByte(21) <> 170 THEN
    CALL TDF.Close
    ERROR "TDF: not a TheDraw font"
  END IF
END SUB

' Parse the font record that starts at absolute offset base%.
SUB TDF.ReadRecord(base%)
  LOCAL i%, c%, lo%, hi%, o%, nl%
  TDF.name$ = ""
  nl% = TDF.AtByte(base% + 4)
  FOR i% = 0 TO nl% - 1
    c% = TDF.AtByte(base% + 5 + i%)
    IF c% = 0 THEN EXIT FOR
    TDF.name$ = TDF.name$ + CHR$(c%)
  NEXT i%
  TDF.type% = TDF.AtByte(base% + 21)
  TDF.spacing% = TDF.AtByte(base% + 22)
  TDF.data% = base% + TDF_RECORD%
  TDF.height = 0
  FOR i% = 0 TO TDF_GLYPHS% - 1
    lo% = TDF.AtByte(base% + 25 + 2 * i%)
    hi% = TDF.AtByte(base% + 26 + 2 * i%)
    o% = lo% + 256 * hi%
    TDF.off%(i%) = o%
    IF o% = TDF_UNDEF% THEN
      TDF.def%(i%) = 0
      TDF.wid%(i%) = 0
      TDF.hgt%(i%) = 0
    ELSE
      TDF.def%(i%) = 1
      TDF.wid%(i%) = TDF.AtByte(TDF.data% + o%)
      TDF.hgt%(i%) = TDF.AtByte(TDF.data% + o% + 1)
      IF TDF.hgt%(i%) > TDF.height THEN TDF.height = TDF.hgt%(i%)
    END IF
  NEXT i%
END SUB

' Load the first font in path$.
SUB TDF.Load(path$)
  CALL TDF.OpenFile(path$)
  CALL TDF.ReadRecord(20)
  TDF.path$ = path$
END SUB

' Load the first font in path$ whose name matches font$ (case-insensitive).
SUB TDF.LoadNamed(path$, font$)
  LOCAL base%, blk%, last%, found%, i%, c%, nl%, nm$
  CALL TDF.OpenFile(path$)
  base% = 20
  last% = LOF(#TDF.fn%)
  found% = 0
  DO WHILE base% + TDF_RECORD% <= last%
    nm$ = ""
    nl% = TDF.AtByte(base% + 4)
    FOR i% = 0 TO nl% - 1
      c% = TDF.AtByte(base% + 5 + i%)
      IF c% = 0 THEN EXIT FOR
      nm$ = nm$ + CHR$(c%)
    NEXT i%
    IF UCASE$(nm$) = UCASE$(font$) THEN
      found% = 1
      EXIT DO
    END IF
    blk% = TDF.AtByte(base% + 23) + 256 * TDF.AtByte(base% + 24)
    base% = base% + TDF_RECORD% + blk% + 1
  LOOP
  IF found% = 0 THEN
    CALL TDF.Close
    ERROR "TDF: font not found: " + font$
  END IF
  CALL TDF.ReadRecord(base%)
  TDF.path$ = path$
END SUB

' Advance for a space or an undefined glyph. TheDraw has no space glyph, so a
' space moves the cursor by the font's letter spacing (at least one cell).
FUNCTION TDF.Gap()
  IF TDF.spacing% > 1 THEN
    TDF.Gap = TDF.spacing%
  ELSE
    TDF.Gap = 1
  END IF
END FUNCTION

' Draw glyph idx% (0-based, ASCII 33..126) with its top-left cell at x%, y%.
SUB TDF.DrawGlyph(x%, y%, idx%)
  LOCAL r%, col%, code%, attr%, ch%
  r% = 0
  SEEK #TDF.fn%, TDF.data% + TDF.off%(idx%) + 2
  DO
    col% = 0
    ch% = -1
    DO
      IF TDF.type% = 2 THEN
        ch% = TDF.NextByte()
        IF ch% = 0 THEN EXIT DO
        IF ch% = 13 THEN EXIT DO
        attr% = TDF.NextByte()
      ELSE
        ch% = TDF.NextByte()
        IF ch% = 0 THEN EXIT DO
        IF ch% = 13 THEN EXIT DO
        attr% = 0
      END IF
      IF TDF.type% = 0 THEN code% = TDF.OutlineCode(ch%) ELSE code% = ch%
      IF NOT (TDF.type% = 0 AND ch% = 38) THEN
        IF TDF.type% = 2 THEN COLOUR attr% MOD 16, attr% \ 16
        LOCATE y% + r%, x% + col%
        PRINT CHR$(code%);
      END IF
      col% = col% + 1
    LOOP
    IF ch% = 0 THEN EXIT DO
    r% = r% + 1
    IF r% > 63 THEN EXIT DO
  LOOP
END SUB

' Draw s$ at character cell (x% = column, y% = row).
'
' MMBasic AND does not short-circuit, and nested IF/ELSE blocks do not nest
' reliably, so the glyph table is only indexed after a single-line guard has
' established idx% is in range. A space (ASCII 32), and any undefined or
' out-of-range glyph, advances by TDF.Gap() without touching TDF.def%.
SUB TDF.Print(x%, y%, s$)
  LOCAL i%, c%, idx%, cx%, has%
  IF TDF.open% = 0 THEN ERROR "TDF: no font loaded"
  cx% = x%
  FOR i% = 1 TO LEN(s$)
    c% = ASC(MID$(s$, i%, 1))
    IF c% >= TDF_FIRST% AND c% <= TDF_LAST% THEN
      idx% = c% - TDF_FIRST%
    ELSE
      idx% = -1
    END IF
    has% = 0
    IF idx% >= 0 THEN has% = TDF.def%(idx%)
    IF has% = 1 THEN
      CALL TDF.DrawGlyph(cx%, y%, idx%)
      cx% = cx% + TDF.wid%(idx%) + TDF.spacing%
    ELSE
      cx% = cx% + TDF.Gap()
    END IF
  NEXT i%
  LOCATE y%, cx%
END SUB

' Columns that TDF.Print would advance for s$, without drawing.
FUNCTION TDF.Width(s$)
  LOCAL i%, c%, idx%, w%, has%
  w% = 0
  FOR i% = 1 TO LEN(s$)
    c% = ASC(MID$(s$, i%, 1))
    IF c% >= TDF_FIRST% AND c% <= TDF_LAST% THEN
      idx% = c% - TDF_FIRST%
    ELSE
      idx% = -1
    END IF
    has% = 0
    IF idx% >= 0 THEN has% = TDF.def%(idx%)
    IF has% = 1 THEN
      w% = w% + TDF.wid%(idx%) + TDF.spacing%
    ELSE
      w% = w% + TDF.Gap()
    END IF
  NEXT i%
  TDF.Width = w%
END FUNCTION
