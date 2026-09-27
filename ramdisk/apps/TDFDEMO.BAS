' TDFDEMO.BAS - draw a banner with a native TheDraw font.
' Run with:  RUN "A:/apps/TDFDEMO.BAS"

CLS RGB(0, 0, 0)
COLOUR RGB(255, 255, 255), RGB(0, 0, 0)

TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF"
PRINT "Font: "; TDF.NAME$; "  type "; TDF.TYPE%
TDF PRINT 0, 3, "MMBASIC"
TDF CLOSE
