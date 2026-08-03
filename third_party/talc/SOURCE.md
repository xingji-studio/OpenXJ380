# talc allocator provenance

- Upstream: https://github.com/SFBdragon/talc
- Upstream commit: `ad7bae44cec09e810d531b558499bef47ca4d17b`
- Upstream version observed: `5.0.4`
- Upstream license: MIT
- Upstream license file: `third_party/talc/LICENSE.md`
- Local archive: `liballoc-x86_64.a`
- Local archive SHA-256: `2137fa65410bfecc22371769f3557e8f342216254afe67ad72e76117d1446d08`

`liballoc-x86_64.a` contains Rust v0 symbols that name the `talc` crate, for
example `_RNvMs0_NtCs1PPGXRNTyC3_4talc4talc...`. The archive is kept as a
prebuilt runtime input, so this record carries the talc MIT notice alongside the
existing Rust `alloc` and `compiler_builtins` notices.
