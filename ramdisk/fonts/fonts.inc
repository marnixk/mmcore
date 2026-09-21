' FONTS.INC - bitmap font descriptions. Each font has A:/fonts/<name>.png
' and A:/fonts/<name>.json. Load one with:  f = fontLoad("08X08-F1")

TYPE FontDescription
  source AS STRING
  charset AS STRING
  offsetX AS INTEGER
  offsetY AS INTEGER
  charWidth AS INTEGER
  charHeight AS INTEGER
  charsPerRow AS INTEGER
  bgColour AS INTEGER
END TYPE

FUNCTION fontLoad(name$) AS FontDescription
  LOCAL f AS FontDescription
  LOCAL js$
  OPEN "A:/fonts/" + name$ + ".json" FOR INPUT AS #1
  LINE INPUT #1, js$
  CLOSE #1
  JSON_PARSE js$, f
  STRUCT COPY f, fontLoad
END FUNCTION
