' TDF.BAS - TheDraw (.TDF) font loader and banner printer for MMBasic.
'
' TheDraw fonts are seeded at A:/fonts/tdf/ (see issue #554), grouped into
' mono/, color/ and deco/ categories. Include this library, load a font, then
' draw strings in TEXT mode:
'
'   #INCLUDE "A:/lib/TDF.BAS"
'   TDF.Load "A:/fonts/tdf/mono/STANDARD.TDF"
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
' The whole file is read into a byte buffer once at load, so parsing and
' drawing never touch the file again. MMBasic strings are NUL-terminated and
' this format is full of 0x00 bytes, so the buffer is a numeric array rather
' than a string; the file is closed as soon as it is buffered.
'
' A .TDF may pack several variations of one font (same glyphs, different colour
' palettes). TDF.Variants% is the count, TDF.VariantName$(n%) names the n%th
' (0-based) record, TDF.LoadVariant path$, n% loads it, and TDF.variant% is the
' index currently loaded. TDF.Load loads variant 0.
'
' Glyph rows are read until the NUL terminator, so descender ('&') lines are
' kept even when the Max Height header under-counts them. Color fonts use a
' (character, attribute) byte pair; attribute 00 is a legal colour, only a NUL
' character ends the glyph. Outline fonts are remapped from A-O / @ / & to the
' CP437 box-drawing cells documented by Roy/SAC:
'   https://www.roysac.com/blog/2014/04/thedraw-fonts-file-tdf-specifications/

CONST TDF_FIRST% = 33
CONST TDF_LAST% = 126
CONST TDF_GLYPHS% = 94
CONST TDF_RECORD% = 213
CONST TDF_UNDEF% = 65535
CONST TDF_CHUNK% = 256
CONST TDF_MAX_VARIANTS% = 64

DIM TDF.path$
DIM TDF.name$
DIM TDF.buf%(0)
DIM TDF.size%
DIM TDF.type%
DIM TDF.spacing%
DIM TDF.height
DIM TDF.data%
DIM TDF.open%
DIM TDF.variants%
DIM TDF.variant%
DIM TDF.rec%(TDF_MAX_VARIANTS% - 1)
DIM TDF.VariantName$(TDF_MAX_VARIANTS% - 1)
DIM TDF.off%(93)
DIM TDF.wid%(93)
DIM TDF.hgt%(93)
DIM TDF.def%(93)

TDF.open% = 0
TDF.size% = 0
TDF.variants% = 0
TDF.variant% = 0
TDF.name$ = ""
TDF.path$ = ""
TDF.spacing% = 1
TDF.height = 0

' Read one byte from the in-memory buffer. A NUL byte and an out-of-range
' position both read as 0.
FUNCTION TDF.AtByte(pos%)
  IF pos% < 0 OR pos% >= TDF.size% THEN
    TDF.AtByte = 0
  ELSE
    TDF.AtByte = TDF.buf%(pos%)
  END IF
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
  TDF.open% = 0
  TDF.size% = 0
  TDF.variants% = 0
  TDF.variant% = 0
  TDF.name$ = ""
END SUB

' Walk the font records from the buffer, index each in TDF.rec%() and record its
' name in TDF.VariantName$(). A truncated or garbage tail simply ends the walk,
' so the good leading records remain usable.
' (LOCAL names are global in this MMBasic, so use cr* prefixes to avoid
' clobbering a caller's variables.)
SUB TDF.CountRecords
  LOCAL crbase%, crbs%, crnxt%, crb0%, crb1%, crb2%, crb3%
  LOCAL cri%, crc%, crnl%, crnm$
  TDF.variants% = 0
  crbase% = 20
  DO WHILE TDF.variants% < TDF_MAX_VARIANTS%
    IF crbase% + TDF_RECORD% > TDF.size% THEN EXIT DO
    crb0% = TDF.AtByte(crbase%)
    crb1% = TDF.AtByte(crbase% + 1)
    crb2% = TDF.AtByte(crbase% + 2)
    crb3% = TDF.AtByte(crbase% + 3)
    IF crb0% <> 85 THEN EXIT DO
    IF crb1% <> 170 THEN EXIT DO
    IF crb2% <> 0 THEN EXIT DO
    IF crb3% <> 255 THEN EXIT DO
    TDF.rec%(TDF.variants%) = crbase%
    crnm$ = ""
    crnl% = TDF.AtByte(crbase% + 4)
    FOR cri% = 0 TO crnl% - 1
      crc% = TDF.AtByte(crbase% + 5 + cri%)
      IF crc% = 0 THEN EXIT FOR
      crnm$ = crnm$ + CHR$(crc%)
    NEXT cri%
    TDF.VariantName$(TDF.variants%) = crnm$
    TDF.variants% = TDF.variants% + 1
    crbs% = TDF.AtByte(crbase% + 23) + 256 * TDF.AtByte(crbase% + 24)
    crnxt% = crbase% + TDF_RECORD% + crbs%
    IF crnxt% + 4 <= TDF.size% THEN
      crb0% = TDF.AtByte(crnxt%)
      IF crb0% <> 85 THEN crnxt% = crnxt% + 1
    END IF
    crbase% = crnxt%
  LOOP
  IF TDF.variants% = 0 THEN ERROR "TDF: no font records"
END SUB

' Open path$, read the whole file into TDF.buf%, close it, and check the
' TheDraw header. File stream #10 is only held while buffering.
' (LOCAL names are global here, so use of* to avoid clobbering callers such as
' TDF.LoadVariant's n% argument.)
SUB TDF.OpenFile(path$)
  LOCAL offn%, ofsz%, ofbase%, ofn%, ofchunk$, ofm%, ofi%, ofbufp%, ofc%
  IF TDF.open% THEN CALL TDF.Close
  offn% = 10
  OPEN path$ FOR INPUT AS #offn%
  ofsz% = LOF(#offn%)
  IF ofsz% < TDF_RECORD% THEN
    CLOSE #offn%
    ERROR "TDF: file not found or too small"
  END IF
  REDIM TDF.buf%(ofsz% - 1)
  ofbase% = 0
  ofbufp% = 0
  DO WHILE ofbase% < ofsz%
    SEEK #offn%, ofbase%
    ofn% = TDF_CHUNK%
    IF ofbase% + ofn% > ofsz% THEN ofn% = ofsz% - ofbase%
    ofchunk$ = INPUT$(ofn%, offn%)
    ofm% = LEN(ofchunk$)
    IF ofm% > 0 THEN
      FOR ofi% = 1 TO ofm%
        TDF.buf%(ofbufp%) = ASC(MID$(ofchunk$, ofi%, 1))
        ofbufp% = ofbufp% + 1
      NEXT ofi%
    END IF
    IF ofm% < ofn% THEN
      TDF.buf%(ofbufp%) = 0
      ofbufp% = ofbufp% + 1
      ofbase% = ofbase% + ofm% + 1
    ELSE
      ofbase% = ofbase% + ofn%
    END IF
  LOOP
  CLOSE #offn%
  TDF.size% = ofsz%
  TDF.open% = 1
  IF TDF.AtByte(0) <> 19 THEN ERROR "TDF: not a TheDraw font"
  IF TDF.AtByte(19) <> 26 THEN ERROR "TDF: not a TheDraw font"
  IF TDF.AtByte(20) <> 85 THEN ERROR "TDF: not a TheDraw font"
  IF TDF.AtByte(21) <> 170 THEN ERROR "TDF: not a TheDraw font"
  FOR ofi% = 0 TO 17
    ofc% = ASC(MID$("TheDraw FONTS file", ofi% + 1, 1))
    IF TDF.AtByte(1 + ofi%) <> ofc% THEN ERROR "TDF: not a TheDraw font"
  NEXT ofi%
  CALL TDF.CountRecords
END SUB

' Parse the font record that starts at absolute offset base% (name excluded;
' the caller sets TDF.name$ from TDF.VariantName$()).
' (LOCAL names are global here, so use rr* to avoid clobbering callers.)
SUB TDF.ReadRecord(base%)
  LOCAL rri%, rrlo%, rrhi%, rro%
  TDF.type% = TDF.AtByte(base% + 21)
  TDF.spacing% = TDF.AtByte(base% + 22)
  TDF.data% = base% + TDF_RECORD%
  TDF.height = 0
  FOR rri% = 0 TO TDF_GLYPHS% - 1
    rrlo% = TDF.AtByte(base% + 25 + 2 * rri%)
    rrhi% = TDF.AtByte(base% + 26 + 2 * rri%)
    rro% = rrlo% + 256 * rrhi%
    TDF.off%(rri%) = rro%
    IF rro% = TDF_UNDEF% THEN
      TDF.def%(rri%) = 0
      TDF.wid%(rri%) = 0
      TDF.hgt%(rri%) = 0
    ELSE
      TDF.def%(rri%) = 1
      TDF.wid%(rri%) = TDF.AtByte(TDF.data% + rro%)
      TDF.hgt%(rri%) = TDF.AtByte(TDF.data% + rro% + 1)
      IF TDF.hgt%(rri%) > TDF.height THEN TDF.height = TDF.hgt%(rri%)
    END IF
  NEXT rri%
END SUB

' Load variation n% (0-based) from path$.
SUB TDF.LoadVariant(path$, n%)
  IF n% < 0 THEN ERROR "TDF: bad variant"
  CALL TDF.OpenFile(path$)
  IF n% >= TDF.variants% THEN
    CALL TDF.Close
    ERROR "TDF: no such variant"
  END IF
  CALL TDF.ReadRecord(TDF.rec%(n%))
  TDF.name$ = TDF.VariantName$(n%)
  TDF.variant% = n%
  TDF.path$ = path$
END SUB

' Load the first font in path$.
SUB TDF.Load(path$)
  CALL TDF.LoadVariant(path$, 0)
END SUB

' Load the first font in path$ whose name matches font$ (case-insensitive).
SUB TDF.LoadNamed(path$, font$)
  LOCAL lni%
  CALL TDF.OpenFile(path$)
  FOR lni% = 0 TO TDF.variants% - 1
    IF UCASE$(TDF.VariantName$(lni%)) = UCASE$(font$) THEN
      CALL TDF.ReadRecord(TDF.rec%(lni%))
      TDF.name$ = TDF.VariantName$(lni%)
      TDF.variant% = lni%
      TDF.path$ = path$
      EXIT SUB
    END IF
  NEXT lni%
  CALL TDF.Close
  ERROR "TDF: font not found: " + font$
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
  LOCAL r%, col%, code%, attr%, ch%, p%
  r% = 0
  p% = TDF.data% + TDF.off%(idx%) + 2
  DO
    col% = 0
    ch% = -1
    DO
      IF TDF.type% = 2 THEN
        ch% = TDF.AtByte(p%) : p% = p% + 1
        IF ch% = 0 THEN EXIT DO
        IF ch% = 13 THEN EXIT DO
        attr% = TDF.AtByte(p%) : p% = p% + 1
      ELSE
        ch% = TDF.AtByte(p%) : p% = p% + 1
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
