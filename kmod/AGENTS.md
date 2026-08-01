# KMOD KNOWLEDGE BASE

## OVERVIEW
Loadable kernel module subtree. Modules build as shared `.sys` artifacts, load through the kernel dynamic linker, and are copied into the disk image `/mod`.

## STRUCTURE
```
kmod/
├── e1000/           # Intel E1000 network driver module
├── netserver/       # lwIP-backed networking module
├── xhci/            # USB xHCI module
└── dlinker_test/    # Module dependency/linker test, not built by default
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Module policy | `README.md` | Boundary rule for `kmod/` vs `kernel/` vs `driver/`. |
| Module loader | `kernel/dlinker.cpp`, `include/dlinker.h` | Loads `.sys`, resolves symbols/deps. |
| Top-level build | `tools/gen_ninja.py` kmod section | Builds e1000, netserver, and xhci artifacts. |
| Module entry | `*/*.cpp` | Linker entry symbol is `dlmain`. |
| Image copy | `tools/ninja_build.py` | Copies `out/*.sys` into image `/mod`. |

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
- Do not assume `.sys` files land under `/system`; root image copy places `out/*.sys` into `/mod`.
- Do not edit module object order casually where the generated graph hardcodes link order.

## VERIFY
Use `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja kmods` for module-only builds; `ninja -f build.ninja all` and `ninja -f build.ninja vdisk` verify image staging.
