# Native ELF dynamic linking: first milestone

The native x86-64 ELF interpreter is `/system/lib/ld-xj380-x86_64.so.1`.
The kernel maps the executable and interpreter, builds the initial stack and
auxiliary vector, and transfers control to the interpreter. Native execution
retains the XAPI syscall ABI. Existing Linux interpreters retain their Linux
ABI selection; static ELF and EPF keep their existing entry paths.

`user/rtld/` contains the freestanding interpreter, a dynamic CRT, the native
ABI note and a small shared-library example. This common implementation is
built by both the public and product Ninja generators through
`tools/native_rtld.py`. It has no libc, allocator archive or kernel symbol-table
dependency. The existing kernel module linker remains separate.

## Build and install

Run from the desired project root using the project's Ninja wrapper:

```sh
python3 tools/gen_ninja.py --out build.ninja
./ninja native_rtld
./ninja all
```

For the public subproject from the product checkout, use `./ninja -C OpenXJ380`.
The outputs are:

| Build artifact | Installed path |
| --- | --- |
| `out/ld-xj380-x86_64.so.1` | `/system/lib/ld-xj380-x86_64.so.1` |
| `out/libnative-demo.so.1` | `/system/lib/libnative-demo.so.1` |
| `out/native-dynamic-demo.elf` | `/apps/native-dynamic-demo/native-dynamic-demo.elf` |

Full image staging and installer temporary roots install all three. The
`/system/lib` runtime directory is publicly readable; writes continue to use
the protected-system permission policy. Resolved aliases cannot grant access
to protected files outside that directory. No loader configuration file or
implicit current-directory search is introduced.

The product checkout additionally provides editable sources under
`user/rtld_demo/` and the `./ninja rtld_demo` target. Image and installer staging
place its program at `/user/rtld-demo.elf`, its library at
`/user/librtld-demo.so.1`, and its guide at `/user/README-rtld-demo.md`.
The program's RUNPATH is `$ORIGIN`, so this example verifies a library next to
the executable. It prints addresses and individual checks to stdout and serial;
an optional output-file argument preserves the report including finalization.
Use `python3 tests/run_native_rtld_qemu.py --user-demo` to verify this placement
and library search on a disposable image.

## Native ELF ABI v1 profile

Executables, libraries and the interpreter carry a `PT_NOTE` entry named
`XJ380\0`, type 1, with a four-byte little-endian version number of 1. Libraries
are ELF64, little endian, x86-64 `ET_DYN`, with readable, separate `PT_LOAD`
pages and no writable executable segment. Main programs supply `PT_PHDR` and
use the dedicated interpreter path. The kernel validates native ELF image
ranges, alignment, ownership and initial page permissions before entry.

The interpreter bootstraps its own `RELATIVE` relocations, then reads the main
program from `AT_PHDR/PHENT/PHNUM`, `AT_BASE` and `AT_ENTRY`. `AT_EXECFN` names
the executable independently of `argv[0]`. The dynamic CRT reads arguments and
environment from the initial stack, calls `main(argc, argv, envp)`, calls the
interpreter finalizer supplied in RDX, and exits with main's status.

Supported features:

- `PT_DYNAMIC`, `DT_NEEDED`, SONAME, SysV and GNU hash tables; section headers
  are optional. Dependencies are discovered breadth first and loaded once per
  process. Symbol lookup preserves that dependency order and visibility.
- `R_X86_64_RELATIVE`, `R_X86_64_64`, `R_X86_64_GLOB_DAT`,
  `R_X86_64_JUMP_SLOT` and `NONE`, with immediate binding and undefined weak
  imports resolved to zero. `DT_SYMBOLIC`/`DF_SYMBOLIC` use local definitions.
- Colon-separated RUNPATH (or RPATH when RUNPATH is absent), `$ORIGIN`, and
  fallback `/system/lib`. Names containing `/` are used as paths. Environment
  search overrides and preloading are not implemented.
- Zero-filled BSS, segment permissions and RELRO after relocation; preinit,
  dependency initialization, init/fini arrays and reverse finalization.
  Constructors and finalizers run in user mode. Raw `_exit`-style syscalls
  bypass finalization.

The bounded profile permits 32 objects, 64 program headers per object, 32
dependencies per object, 65536 symbols, 32 MiB library files and 64 MiB image
spans. Unsupported relocations and necessary features fail with an explanatory
stderr diagnostic and exit status 127. Libraries need not have an entry point.

This milestone provides startup linking and its example, while shared XAPI,
`dlopen/dlsym/dlclose`, TLS, lazy PLT binding, COPY/IFUNC, symbol versions and
general C++ runtime integration remain subsequent work. Existing applications
still use their established runtime and link rules. Text-page sharing between
processes and ELF ASLR are not claimed by this milestone.

## Verification

From the product checkout:

```sh
python3 -m unittest discover -s tests -p test_native_rtld.py
python3 -m unittest discover -s tests -p test_elf_loader.py
python3 -m unittest discover -s tests -p test_permissions.py
python3 tests/run_native_rtld_qemu.py
```

The host tests execute toolchain-generated binaries through the real
interpreter, including stripped section headers, both hash formats, weak and
missing imports, malformed libraries, constructors/finalizers and concurrent
private data. The kernel adapter runs the production loading functions under
ASan/UBSan with preemption and allocation/permission failure injection.

The QEMU runner uses four CPUs and modifies only a temporary FAT image clone.
It checks two simultaneous native processes, native exec, missing dependencies,
failed exec rollback, static ELF launches, and persisted constructor/
relocation/BSS/isolation/finalizer results. When the separate EPF converter is
available it also checks EPF launches; the JSON result records whether that
regression ran. `--no-epf` explicitly selects ELF-only verification.
`--trace-loader` builds a diagnostic
interpreter only for that clone. The original image and launch configuration
are preserved.
