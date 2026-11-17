# KMOD KNOWLEDGE BASE

## OVERVIEW
Loadable kernel module subtree. Modules build as shared `.sys` artifacts, load through the kernel dynamic linker, and are copied into the disk image `/system/mod`.

## STRUCTURE
```
kmod/
├── e1000/           # Intel E1000 network driver module
├── rtl8139/         # Realtek RTL8139 network driver module
├── virtio-net/      # Legacy/transitional VirtIO network driver module
├── netserver/       # lwIP-backed networking module
├── xhci/            # USB xHCI module
└── dlinker_test/    # Module dependency/linker test, not built by default
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Module policy | `README.md` | Boundary rule for `kmod/` vs `kernel/` vs `driver/`. |
| Module loader | `kernel/dlinker.cpp`, `include/dlinker.h` | Loads `.sys`, resolves symbols/deps. |
| Top-level build | `tools/kernel_modules.py` | Shared sources, compiler/linker rules, and image artifact list. |
| Module entry | `*/*.cpp` | Linker entry symbol is `dlmain`. |
| Image copy | `tools/ninja_build.py` | Copies `out/*.sys` into image `/system/mod`. |

## CONVENTIONS
- Put code here only if it is intended to be linked as a shared loadable kernel module.
- Module C/C++ flags are kernel-like but add `-fPIC` and `-fvisibility=hidden`.
- Module link commands use `-shared` and `-Wl,-e,dlmain`; some modules also use `-zmuldefs` or `-Bsymbolic`.
- Export/import shared kernel symbols through the existing dlinker/kernel symbol mechanism.
- `dlinker_test` is not built by the normal generated Ninja graph.
- Keep vendored code inside module subtrees documented as integration only; avoid local edits to upstream internals.

## ANTI-PATTERNS
- Do not put mandatory boot drivers in `kmod/`; safe mode/boot flags can disable module loading.
- Do not create Linux-style `init_module` expectations; this repo uses `dlmain`.
- Do not load `xhci.sys` when `BUILTIN_XHCI=1`; staging excludes the artifact in this mode.
- Do not duplicate common module sources or rules in the parent product.
- Do not edit module object order casually where the generated graph hardcodes link order.

## VERIFY
Generate this repository's Ninja graph with `python3 tools/gen_ninja.py --out build.ninja`. In the parent checkout, use its guarded `./ninja -C OpenXJ380 -f build.ninja kmods` from the parent root. Verify both `BUILTIN_XHCI` modes and preserve the five allocator wrap flags in both generators.
