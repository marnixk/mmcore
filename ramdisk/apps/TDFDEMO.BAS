' TDFDEMO.BAS - draw a banner with a TheDraw font (A:/lib/TDF.BAS).
' Run with:  RUN "A:/apps/TDFDEMO.BAS"
#INCLUDE "A:/lib/TDF.BAS"

CLS RGB(0, 0, 0)
COLOUR RGB(255, 255, 255), RGB(0, 0, 0)

TDF.Load "A:/fonts/tdf/mono/STANDARD.TDF"
PRINT "Font: "; TDF.Name$; "  type "; TDF.Type%
TDF.Print 0, 3, "MMBASIC"
TDF.Close
