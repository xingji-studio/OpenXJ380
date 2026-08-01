Text rendering internals live in this subtree.

Planned split:
- `font.hpp`: public font object used by themes and widgets
- `rasterizer/`: glyph rasterization backend, intended for `ab_glyph_rasterizer` port
- `truetype/`: font outline parsing and glyph extraction
