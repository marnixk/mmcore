# A:/lib

Shared BASIC include files seeded from `ramdisk/lib/`. Use them with
`#include "lib/NAME.INC"` or `CHAIN`.

## TDF.BAS

`TDF.BAS` loads the TheDraw `.TDF` fonts in `A:/fonts/tdf/` (categories `mono/`,
`color/`, `deco/`) and draws banner text in TEXT mode. It is an include file:

```basic
#INCLUDE "A:/lib/TDF.BAS"
TDF.Load "A:/fonts/tdf/mono/STANDARD.TDF"
TDF.Print 0, 3, "HELLO"
TDF.Close
```

`TDF.Load` reads the first font in a file; `TDF.LoadNamed path$, name$` selects
one font by name from a collection. After a load, `TDF.Name$`, `TDF.Type%`
(0 Outline, 1 Block, 2 Color), `TDF.Spacing%` and `TDF.Height` describe the
font, and `TDF.Width(s$)` is the column advance for a string. `TDF.Print x, y,
s$` draws at character cell (column, row). A file can hold several colour
variations of one font: `TDF.Variants%` is the count, `TDF.VariantName$(n%)`
names one, and `TDF.LoadVariant path$, n%` loads it. The whole file is
buffered into memory at load and the file is closed immediately; see `HELP TDF`.
