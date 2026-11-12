# Native ELF dynamic linking

The native x86-64 ELF interpreter is `/system/lib/ld-xj380-x86_64.so.1`.
The kernel maps the executable and interpreter, builds the initial stack and
auxiliary vector, and transfers control to the interpreter. Native execution
retains the XAPI syscall ABI. Existing Linux interpreters retain their Linux
ABI selection; static ELF and EPF keep their existing entry paths.

`user/rtld/` contains the freestanding interpreter, a dynamic CRT, the native
ABI note, TLS/thread runtime and shared-library examples. This common implementation is
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

Full image staging and installer temporary roots install these artifacts and the
TLS/runtime artifacts described below. The
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

The first milestone provides startup linking and its example. The shared XAPI
milestone below adds a runtime usable by ordinary native applications.
`dlopen/dlsym/dlclose`, lazy PLT binding, COPY/IFUNC, symbol versions and
general C++ runtime integration remain subsequent work. TLS and the supported
C++ `thread_local` subset are implemented by the milestones below. Existing applications
still use their established runtime and link rules. Text-page sharing between
processes and ELF ASLR are not claimed by this milestone.

## Second milestone: shared XAPI

`./ninja shared_xapi` builds `out/libxapi.so.1` from separate PIC objects and
`out/rtld/xapi_crt0.S.o`. Static XAPI objects and entry shims retain their build
rules. The system image and installer roots install the shared runtime at
`/system/lib/libxapi.so.1`.

The product runtime reuses its existing core libc/POSIX, permission, terminal,
and GUI syscall wrappers. It includes the existing heap, standard streams,
`errno` and `environ`; applications and their dependent libraries use one copy
per process. Its explicit source list is `PRODUCT_XAPI_SOURCES` in
`tools/native_rtld.py`. Network/TLS and media archives retain their independent
build and dependency scopes. The public tree supplies its existing minimal
syscall API and a small `xapi-shared-demo.elf`, without importing product code.
This profile distinction matches the existing static runtimes.

After relocation and page protection, the interpreter looks up
`__xapi_rtld_init_v1` **in the object whose SONAME is `libxapi.so.1`**. It passes
`argc`, `argv`, `envp` and the loader finalizer before any preinit or constructor.
A runtime with that SONAME must export this versioned function. It initializes
`environ` and records the finalizer. The shared CRT subsequently calls
`__xapi_start_main` with the same data and the application main function.
Initialization is idempotent, so constructor changes to the environment survive
entry to main. A standalone startup demo without shared XAPI keeps the original
dynamic CRT and does not use this handshake.

Returning from main or calling shared `exit()` runs loader finalizers exactly
once before exiting the process, including exit from a main constructor.
`_exit` and raw exit syscalls bypass finalizers. This step supplies ELF
init/fini lifecycle integration; it does not add `atexit`, `__cxa_atexit`,
or thread-safe libc state. TLS lifecycle integration is described below. `errno` retains its existing process-global model.

Use `-fPIC` for both application and library objects, and link applications as
PIE with the shared CRT, native ABI note, and `libxapi.so.1`. PIC avoids COPY
relocations and preserves a single address for imported data such as `errno`.
All shared outputs use immediate binding, RELRO, separate LOAD pages and a
non-executable stack; `libxapi.so.1` has no external library dependency.

The product example is installed as `/user/xapi-demo.elf` with its companion
`/user/libxapi-demo.so.1` and `/user/README-xapi-demo.md`. It checks constructor
and preinit environment, shared errno/environment, POSIX error translation,
allocation and free across ELF objects, file I/O, process isolation and ordered
finalization. `./ninja all` includes this runtime and example in both builds.

## Third milestone: native ELF TLS

`./ninja native_tls` builds the interpreter, `libnative-runtime.so.1`,
`libnative-tls.so.1`, `libnative-tls-ie.so.1` and `native-tls-demo.elf`.
`./ninja all` includes them in both project graphs. Image and installer staging
place libraries in `/system/lib`, demos in `/apps/native-dynamic-demo`, and the
public `native_runtime.h` in `/system/include`.

The native ABI note now records the minimum required runtime capability:

| Note version | Required capability |
| --- | --- |
| 1 | Original native ELF dynamic linking and shared XAPI |
| 2 | TLS layout, access and native worker threads |
| 3 | C++ thread-local destructor registration and exit lifecycle |

The interpreter advertises version 3 and accepts versions 1 through 3. The
interpreter path and `.so.1` names are unchanged. Objects whose note understates
TLS/C++ requirements, or exceeds the interpreter's capability, fail before
constructors with exit 127. Compile `abi_note.S` with
`-DXJ380_NATIVE_ABI=2` for TLS objects and `=3` for nontrivial C++ TLS objects;
the default remains 1 for existing callers. The runtime DSO advertises 3.
This profile applies to the dedicated native interpreter, not Linux binaries
or legacy static ELF startup.

Supported TLS features:

- One validated `PT_TLS` per object, initialized `.tdata`, zero-filled `.tbss`,
  and power-of-two alignment up to 2 MiB. The template must start at that
  alignment and match a readable file-backed LOAD range; TLS BSS need not fit
  in ordinary image storage. Each module permits 1 MiB of TLS, all modules
  together 16 MiB. The existing 32-object limit includes modules without TLS.
- x86-64 variant-II layout below FS, main nearest the thread pointer, with
  stable module IDs in the original breadth-first dependency order. TLS
  definitions follow the existing native ELF preemption/visibility and
  `DT_SYMBOLIC` rules, including binding a library reference to main's TLS.
- GD and LD through `__tls_get_addr`, IE through `DF_STATIC_TLS` and GOT
  offsets, and linker-resolved LE in the main executable. Dynamic RELA types
  `R_X86_64_DTPMOD64`, `R_X86_64_DTPOFF64`, and `R_X86_64_TPOFF64` are supported.
  TLS symbols carry offsets, not ordinary image addresses. Ordinary relocation
  types cannot address TLS. Unresolved weak GD yields a null address; weak IE
  and TLS descriptors (`TLSDESC`) are rejected.
- The interpreter installs main's FS before the shared XAPI bootstrap, preinit
  or constructors. Worker TLS comes from read-only snapshots taken after
  relocation and before constructors; main's subsequent writes cannot alter
  another thread's initial values. Snapshots preserve pointers relocated in
  `.tdata`. A control block is installed even without compiler TLS.

The interpreter's exported helpers are searched after main and all ordinary
objects. This also resolves an unused `__tls_get_addr` PLT entry left by GNU ld
after relaxing main's access to LE. The normal global lookup order is preserved.
There is no runtime module loading or TLS layout growth in this milestone.

Applications link `libnative-runtime.so.1` and include `native_runtime.h`:

```c
XjThread thread = {0};
int64_t result;
int64_t status = xj_thread_create(&thread, worker, argument);
if (!status) status = xj_thread_join(&thread, &result);
```

A worker has fresh TLS, a 1 MiB user stack with guard pages and its own FS base.
Its entry returns `int64_t`. Join waits for the kernel to quiesce the child,
then reclaims TLS, stack and the user control record and resets the handle.
The caller serializes access to each handle; a single join owns reclamation.
Errors are negative errno values. Raw worker exit before completing the entry
returns `-ECANCELED` from join, preserves the result argument and discards
pending destructor records without invoking them on the joiner's FS. Detach,
cancellation, POSIX pthreads and thread-safe libc state are outside this API.
Kernel TCB/kernel stack retention still follows the process collector's existing
lifetime; join's user-memory reclamation does not change that limitation.

The runtime implementation (`user/rtld/tls.c`, `tls.h`, `thread.S`) is shared
with EPF. Existing `epf_thread_create/join` entry points and the EPF thread-pointer
layout are retained. This does not make EPF and ELF modules interchangeable.

## Fourth milestone: C++ thread_local

`./ninja native_cxx_tls` additionally builds `libnative-cxx-tls.so.1` and
`native-cxx-tls-demo.elf`. Compile application/library C++ objects with `-fPIC`,
`-fno-exceptions`, and `-fno-rtti`. Link one `out/rtld/cxx_abi.c.o` into each
C++ executable/DSO to supply its own hidden `__dso_handle`, and use ABI note 3.
Use the dynamic CRT, or the shared XAPI CRT and `libxapi.so.1` when calling
shared `exit()` or other XAPI functions. No host C++ standard library is linked.

Both GCC and Clang namespace and function-local `thread_local` objects support
nonthrowing dynamic initialization on first use, per-thread guards and explicit
alignment, plus nontrivial destructors. Objects never touched are not constructed.
`__cxa_thread_atexit` and `__cxa_thread_atexit_impl` share the current thread's
private LIFO registration list. Each thread permits 64 pages / 10,816 pending
records; null callbacks, allocation failure or exhaustion return -1 without
losing earlier registrations. No module unload occurs while callbacks are pending.

A normally returning worker drains its destructors before publishing completion,
so join observes destructor side effects. Returning from main or calling shared
XAPI `exit()` drains main's TLS destructors before module FINI. Registrations
created by a destructor also run in LIFO order; callbacks first registered by FINI
are drained afterward. Shared XAPI's existing single finalization guard also
covers exit from a main constructor. Raw process exit bypasses finalization.

This adds no exception unwinding, throwing/retrying TLS initialization, C++
standard library, global-object `__cxa_atexit`, `atexit`, dynamic unload, or
POSIX pthread support. Shared XAPI's `errno`, streams, environment and allocator
retain their existing concurrency properties; TLS support alone does not make
those APIs thread safe.

## Verification

From the product checkout:

```sh
python3 -m unittest discover -s tests -p test_native_rtld.py
python3 -m unittest discover -s tests -p test_shared_xapi.py
python3 -m unittest discover -s tests -p test_native_tls.py
python3 -m unittest discover -s tests -p test_epf_rtld.py
python3 -m unittest discover -s tests -p test_epf_cxx_tls.py
python3 -m unittest discover -s tests -p test_elf_loader.py
python3 -m unittest discover -s tests -p test_permissions.py
python3 tests/run_native_rtld_qemu.py
python3 tests/run_native_rtld_qemu.py --shared-xapi
python3 tests/run_native_rtld_qemu.py --tls
python3 tests/run_native_rtld_qemu.py --cxx-tls
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

The native TLS host suite executes the real native interpreter and compiler
outputs, including GD/LD/IE/LE, main interposition and SYMBOLIC, pure TLS BSS,
relocated templates, malformed TLS records, eight workers, GCC/Clang C++
initialization, reentrant destructors, shared exit and late FINI registrations.
`--tls` and `--cxx-tls` run the corresponding eight-worker probes concurrently
in separate guest processes, including spawn, exec and ordinary format regressions.
