# App icon branding

`mmcore-app-icon.svg` is the single source of truth. It is the `m` from the
mmcore.dev website favicon (`marnixk/mmcore-website`, `public/favicon.svg` at
commit `55d5a12`), keeping that file's chrome gradients exactly, with two
icon-only adjustments:

- a square `viewBox` (`-2.3 -12.5 76 76`) that centres the wide `m` with room
  for its outline;
- a thicker solid black (`#000000`) silhouette outline (`stroke-width="9"`) so
  the glyph reads at 16/32 px on both light and dark desktops.

`mmcore-app-icon.png` is the 1024x1024 master rendered from the SVG. It is
committed because `scripts/gen-appicon.py` is deliberately stdlib-only, so CI
needs no SVG rasteriser. Regenerate it after editing the SVG with:

```bash
rsvg-convert -w 1024 -h 1024 assets/branding/mmcore-app-icon.svg \
    -o assets/branding/mmcore-app-icon.png
```

Every native packaging icon (macOS `.icns`, Windows `.ico`, Linux
AppImage/hicolor PNG, and the SDL RGBA window icon) is generated from this PNG
by `scripts/gen-appicon.py`.

The `mmcore-app-icon-preview-{light,dark}.png` sheets show the generated icon
at 16, 32, 128 and 512 px; regenerate them with:

```bash
python3 scripts/gen-appicon.py --preview assets/branding
```
