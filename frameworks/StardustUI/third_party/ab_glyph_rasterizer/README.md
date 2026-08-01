# ab_glyph_rasterizer

This directory keeps the upstream `ab-glyph` repository as a git submodule and adds a thin C/C++ API wrapper around its `rasterizer` crate.

Layout:

- `upstream/`
  Git submodule containing the upstream `ab-glyph` repository.
- `wrapper/`
  Local Rust wrapper crate exporting C ABI.
- `include/ab_glyph_rasterizer_c.h`
  Stable C header for the wrapper ABI.
- `include/ab_glyph_rasterizer.hpp`
  Minimal C++ RAII wrapper over the C header.

Build the wrapper crate:

```bash
cd frameworks/StardustUI/third_party/ab_glyph_rasterizer/wrapper
cargo build --release
```

Expected outputs include a `staticlib` and a `cdylib` suitable for integration from C/C++.
