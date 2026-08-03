# Rust runtime archive source record

`liballoc-x86_64.a` is a prebuilt Rust no_std runtime archive used by the
freestanding build. Symbol and object names identify Rust `alloc`, Rust
`compiler_builtins`, and the MIT-licensed `talc` allocator rather than the unrelated
plos-clan/liballoc project.

- Local archive: `liballoc-x86_64.a`
- Archive SHA-256: `2137fa65410bfecc22371769f3557e8f342216254afe67ad72e76117d1446d08`
- Runtime components: Rust `alloc`, Rust `compiler_builtins`, `talc`
- Rust runtime license: Apache-2.0 OR MIT
- talc license: MIT
- License texts: `third_party/rust-runtime/LICENSE-APACHE`,
  `third_party/rust-runtime/LICENSE-MIT`, `third_party/talc/LICENSE.md`

The archive should be regenerated from the Rust target libraries recorded by the
build environment before making a release that needs fully reproducible binary
inputs.
