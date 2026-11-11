# EPF v1.0 native executable loading

`kernel/user/user.cpp` dispatches on the four bytes `24 45 50 46` (`$EPF`),
independently of the filename. The native loader executes fixed-address and
PIE `EPF_EXEC` files. Statically linked images run directly; interpreted
images enter the EPF user linker, which loads `EPF_SHARED` libraries (`.eps`).
An ELF file with a replaced magic is not an EPF file.

Disk fields are decoded by their specified little-endian offsets. The shared
C/C++ definitions live in `include/epf_format.h`; decoded structures are not
serialized or cast over disk bytes. Runtime loading never depends on section
names or section tables. The kernel also validates optional EXEC section
metadata; the user linker checks SHARED section-table bounds without using
sections for loading, symbol lookup or relocation.

## XJ380 x86-64 ABI, versions 1 through 3

These are project-local assignments, not external EPF registry assignments.

| Header field | Accepted value |
|---|---|
| Version, address size, encoding, machine | `1.0`, `8`, `1`, `62` |
| `os_abi`, `abi_version`, `arch_flags` | `0x0380`, `1`, `2` or `3`, `0` |
| Kernel executable type | `2` (`EXEC`) |
| User-linker library type | `3` (`SHARED`), no entry, base zero, flags zero |
| EXEC flags | `0` (fixed) or `1` (PIE, file base zero) |
| Required capabilities | bit 0 `RUNTIME_RELOC`, bit 1 `DYNAMIC_LINK`, bit 2 `TLS`, bit 3 `RELRO` |
| Header / segment / section entry sizes | `128`, `64`, `64` |

Unknown capabilities and flags are rejected. ABI 2 adds native TLS and the
thread runtime. ABI 3 adds C++ TLS destruction on normal thread/main exit.
Compiler TLS needs at least ABI 2; a reference or definition of either
`__cxa_thread_atexit` or `__cxa_thread_atexit_impl` requires ABI 3 and TLS.
Ordinary inputs still default to ABI 1. The current interpreter declares ABI 3,
so older kernels refuse it before user code can silently miss exit callbacks.
ABI 1 and 2 executables and libraries remain accepted by the new loading chain.
The kernel requires an interpreter with an ABI at least as new as its main
image before mapping either image. Thus an ABI 3 main cannot silently use an
older interpreter that understands TLS but omits the main-thread exit hook.
All LOADs are readable, required, and never both writable and executable.
File offsets, RVAs, actual bases and layout alignment obey the specification's
independent alignment constraints. Only LOADs create mappings. BSS and the
extra visible bytes in a final partial page are zero. Full image gaps remain
inaccessible and reserved against later allocation.

Static entry retains the native XAPI convention: `RDI=argc`, `RSI=argv`,
`RDX=envp`. The aligned startup stack contains argc, terminated argv/envp
arrays, auxiliary pairs and the strings. Static `AT_ENTRY` is the entry VA;
`AT_PHDR`, `AT_PHENT`, `AT_PHNUM` and `AT_BASE` are zero. The process uses
native syscalls, not the Linux ABI process mode. Static constructors and
finalizers remain the startup runtime's responsibility.

## Kernel loading and interpreter handoff

A required INTERP descriptor selects exactly
`/system/lib/ld-epf-x86_64.epf`. Its bytes are that single path and final NUL,
with no embedded NUL, `rva=0`, `align=1`, and equal file/memory sizes.
`DYNAMIC_LINK` and INTERP must appear together in an EXEC. A required DYNAMIC
segment is also necessary. The interpreter itself is a statically linked EPF
EXEC with no INTERP; recursive interpretation is rejected. It bootstraps using
only kernel-supported relative relocations, if any.

The kernel maps the main image and interpreter into the explicit target page
directory. All copies use HHDM; the loader never borrows the child's CR3.
Staging pages are writable and NX; final LOAD permissions are applied before
publication. For an interpreted main the kernel applies no runtime relocations
and leaves its declared writable data, including RELRO, writable and NX. The
user interpreter owns all main and library runtime relocation and sealing.

`AT_PHDR` points to a read-only, NX EPF handoff at `0x08000000`, followed by
the original 64-byte segment descriptors. This auxiliary value has EPF
semantics, not ELF program-header semantics. The handoff fields are little-endian:

| Offset | u64 value |
|---|---|
| `0` | Magic `0x31464f4846504524` (`$EPFHOF1`) |
| `8` | Handoff version `1` |
| `16`, `24` | Actual main base and image size |
| `32`, `40` | Actual main entry and segment count |
| `48`, `56` | Required capabilities and mapping alignment |
| `64` onward | Segment descriptors |

`AT_PHENT=64`, `AT_PHNUM` is the main descriptor count, `AT_ENTRY` the main
entry, and `AT_BASE` the interpreter base. The handoff occupies at most five
pages. A collision with this reservation fails loading. Interpreted execution
first enters the interpreter; after preparation it jumps to the main entry
with the original startup stack and `RDX` pointing to the finalizer. Dynamic
programs must use `user/rtld/crt0.S` (or an equivalent EPF-aware CRT), which
reads argc/argv/envp from the stack and calls the finalizer after main returns.
The ordinary static XAPI CRT is not the dynamic CRT. Direct exit syscalls,
forced termination and fatal signals do not run user finalizers.

Main, interpreter and handoff mappings are one preparation transaction.
Mapping, allocation, VMA and protection failures roll back this load's pages
and VMAs and leave old startup metadata unchanged. The existing process
preparation ticket/private exec-image lifetime rules still apply. No PCB or
assembly layout changes or new concurrent-exec lifetime protocol are added.
Linker errors discovered after entering user mode terminate the new process
with status 127; they are not failures returned to the previous exec image.

Fixed EXEC bases must lie between the null guard page and `USER_MMAP_START`,
excluding the message-return stub, legacy ELF-header reservation, stack/mmap
and brk areas. Existing VMAs, mapped pages and lazy reservations cause
collisions. PIE chooses a base in `[0x10000000, 0x100000000-image_size]`,
bounded further by LOAD alignment, signed relative addends and native address
limits. Checked hardware entropy selects the first slot; at most 64 candidates
are examined. No timing/address entropy fallback exists. Missing hardware
entropy returns `-EOPNOTSUPP` for PIE; fixed images do not need entropy.

## Runtime directory, symbols and binding

DYNAMIC is initialized readable LOAD content, with corresponding file offset
and RVA, an aligned directory, a checked NULL terminator and zero tail padding.
The user linker supports all base tags 1 through 14: string and symbol tables,
RELA records, ordered NEEDED entries, informational SONAME and INIT/FINI arrays.
Groups must be complete, strides exact, singletons unique, and all tables
initialized readable bytes. Only NEEDED may repeat. Unknown optional tags can
be skipped; unknown required tags and unsupported attributes fail explicitly.

Libraries are sought only under `/system/lib/epf/`. NEEDED contains a nonempty
ASCII basename of at most 127 bytes, excluding whitespace, slash, backslash,
colon, `.` and `..`. There is no environment, working-directory or RPATH search.
Module identity is its path in this directory with ASCII case folded, matching
the native FAT namespace. Different paths remain different modules even with
the same SONAME; SONAME never selects a replacement file. Path aliases other
than ASCII case variants are distinct identities in this ABI version.

Dependencies are discovered breadth-first in NEEDED order and deduplicated.
Cycles fail before any initializer runs. Dynamic symbol 0 is all zero; other
symbols support LOCAL/GLOBAL/WEAK binding, DEFAULT/HIDDEN visibility,
NOTYPE/OBJECT/FUNC/TLS types and UNDEF/IMAGE/ABS/TLS domains. SECTION
is rejected; TLS definitions use ABI 2 module-relative offsets and section
index zero. Defined IMAGE symbols must fit LOAD memory; functions must point
into executable file content. Symbol names are compared as exact bytes.

Defined references bind the defining module. An undefined reference searches
only its own dependency closure, breadth-first in NEEDED order. Only DEFAULT
GLOBAL/WEAK definitions are candidates. The first strong definition wins over
all weak definitions; without a strong definition the first weak is used.
Missing strong references fail; missing weak references resolve to zero.
Sibling libraries outside that dependency closure are not an ambient scope.
Duplicate strong exports within one module are rejected.

All modules are parsed and checked before relocation writes or user callbacks.
Runtime tables are copied into read-only snapshots, including for an initially
mapped main. Graph-wide relocation values are calculated before applying any
writes. A bounded heapsort rejects overlapping write ranges. Validation and
lookup also have a global work budget, preventing unbounded metadata scans.

## Relocation, RELRO and lifecycle

Without INTERP the kernel supports NONE and symbol-free RELATIVE32/64. An
optional directory supplies the complete RELA_RVA/RELACNT/RELAENT group (tags
6, 7, 8); a nonempty table requires RUNTIME_RELOC. The kernel rejects binding
and lifecycle directory tags that require a user interpreter.

With INTERP the user linker supports all five scalar writes:

| Relocation | Checked result |
|---|---|
| ABS32 / ABS64 | Unsigned `S + signed A`, width 32 / 64 |
| PCREL32 | Signed 32-bit `S + signed A - P` |
| RELATIVE32 / RELATIVE64 | Unsigned `B + signed A`, width 32 / 64, symbol 0 |

NONE writes nothing. Reserved fields must be zero. Integer results are never
truncated. The entire write must fit writable, non-executable LOAD data/BSS,
including scalars crossing pages or adjacent writable LOADs; even a one-byte
semantic gap fails. Writes cannot overlap each other or the directory, string,
symbol, relocation or lifecycle tables. Executable relocations are rejected.
Shared-library bases come from anonymous mmap with the strongest LOAD
alignment. A 32-bit result that cannot represent its actual placement fails;
there is no implicit truncation or forced low-address mapping.

Required RELRO descriptors name disjoint full-page ranges inside writable
LOAD mappings, with zero file offset/size, page alignment and the RELRO feature.
The kernel seals static runtime RELRO after relative relocation and splits its
VMA permissions accordingly. The user linker seals interpreted main/library
RELRO after eager binding and final LOAD protection. No page is ever W+X.
User-copy checks also honor PTE_WRITEABLE before HHDM writes, so a read syscall
cannot bypass RELRO protection.

INIT/FINI arrays contain u64 RVAs, not relocated pointers. Every callback must
point to executable initialized content. Initialization runs complete dependency
layers before their users, preserving first-discovery order within each layer;
module INIT arrays run forward. Finalization reverses actual initialization
order, and each module's FINI array runs backward. No constructor runs if any
module, dependency cycle, symbol, relocation or final protection step fails.

## Native TLS and thread runtime

TLS requires INTERP and ABI 2 or newer, including for a program with no shared-library
dependencies. It is initialized entirely in user mode. The kernel validates the
required TLS descriptor and passes it through the immutable handoff; it never
maps TLS BSS over ordinary image data or invokes TLS constructors.

At most one required TLS segment is allowed per image. `file_size <= mem_size`,
`mem_size` is nonzero and bounded, and `align` is a supported power of two.
Initialized bytes must correspond exactly to readable initialized LOAD bytes.
For zero-file TLS, both file offset and RVA are zero. `mem_size` describes
per-thread storage and can exceed the ordinary image reservation; it is not
another LOAD range. Declaring TLS without an own template is valid for a module
that imports TLS or implements the native TLS/thread API.

The main is placed closest to the thread pointer, followed by dependencies in
discovery order. For each module with storage:

```text
module.end = align_up(previous_end + module.mem_size, module.align)
module storage = thread_pointer - module.end
```

The complete block and thread pointer honor the strongest TLS alignment. This
keeps a linked main's local-exec offsets stable when libraries are added. The
user linker computes this layout before TLS relocation. After all relocations
and final protections, it snapshots initialized TLS bytes into read-only
mappings. Thus pointer initializers in TLS see relocated values, while new
threads start from the original template rather than a modified parent block.
TLS BSS and padding come from zero-filled anonymous mappings.

Module IDs are one-based discovery indices, including modules without storage.
These ABI 2 x86-64 scalar relocation numbers are EPF assignments, not ELF IDs:

| EPF relocation | Number | Result |
|---|---|---|
| X86_64_DTPMOD64 | `0x003e0001` | Defining module ID; addend must be zero |
| X86_64_DTPOFF64 | `0x003e0002` | TLS symbol offset plus signed addend |
| X86_64_TPOFF64 | `0x003e0003` | Signed offset minus defining module.end |

They write eight-byte data fields, never executable instructions. A zero symbol
index refers to the current module. Other indices must resolve to a TLS symbol
using the existing dependency-closure, visibility and strong/weak rules. Offsets
must fit that module's TLS storage. Ordinary address relocations cannot consume
TLS symbols. Undefined weak GD references resolve to module/offset zero and
`__tls_get_addr` returns null; unrepresentable weak IE offsets are rejected.

FS points to a 64-byte native control block, with a DTV immediately following it:

| FS offset | u64 field |
|---|---|
| `0`, `8`, `16` | Self pointer, per-thread module vector, immutable TLS context |
| `24`, `32` | Owned TLS allocation address and size |
| `40` | Process stack canary from getrandom |
| `48`, `56` | Control-block magic and ABI 3 destructor-page head (initially zero) |

DTV element `module_id` is the module's TLS address, or zero for no storage.
The main's FS and complete TLS data are installed before any initializer runs.
The compiler's GD/LD helper `__tls_get_addr` is supplied by the explicit
`libepf-runtime.eps` dependency; no implicit global symbol scope is added.
Initial-exec GOT offsets and a main's already-linked local-exec instructions
use the same layout. TLS, stacks and control records are private to each thread.

`include/epf_runtime.h` exposes the small native thread API:

```c
EpfThread thread = {0};
int64_t result;
epf_thread_create(&thread, worker, argument);
epf_thread_join(&thread, &result);
```

The entry has type `int64_t worker(void *)`. Calls return zero or negative
errno. A handle has one owner, must not be copied, and is serialized by the
caller; exactly one join consumes it and resets it to zero. The argument must
remain valid until join. Result storage is written only after normal callback
completion; a thread terminated before completing reports `-ECANCELED` after
safe reclamation. Self-join returns `-EDEADLK`. There is no detach/cancel API.

Creation allocates a fresh TLS block and a 1 MiB NX stack with inaccessible
pages at both ends. The clone trampoline supplies CLONE_SETTLS before calling
user code. Kernel clone now uses the existing preemptible preparation ticket:
allocation, VFS and TID copies precede its short publication lock; errors roll
back the draft and published TID words. Native ARCH_SET_FS checks user address
bounds and clears the selector so context restoration preserves the FS base.
Both syscall dispatchers connect `sched_yield` to the existing scheduler yield
handler, including the cooperative barriers in the TLS example.
Mmap address selection and publication, munmap VMA/lazy/PTE teardown, brk and
mprotect share the existing preemptible memory-operation mutex. Concurrent TLS
callback mappings and thread joins therefore cannot race over freed VMA nodes
or select the same unreserved range before publication.
The gate also covers lazy-fault lookup, reservation splitting/merging and
removal, and is recursive for faults from user-copy/VFS work. Not-present user
faults restore the inherited zero preemption depth before waiting/allocating,
then close IRQs again before exception return. Faults with inherited disabled
preemption or protection/reserved-bit violations do not enter this slow path.
Concurrent faults recheck an already-present page; new frames are zeroed through
HHDM before their present user PTE is published.
Anonymous mmap permissions use the same checked protection conversion as
mprotect. PROT_NONE guard reservations cannot be faulted into present pages,
and access-denied reservations do not fall back to automatic stack growth.

A TID is cleared only after scheduler references show that the thread cannot
still execute. The reaper also services finished threads in live processes,
using a preparation pin to retain their PCB and directory while doing the
user-memory store outside spin locks. Join then releases user TLS, stack and
control-record mappings. Existing kernel TCB/kernel-stack bookkeeping remains
retained until process collection. This does not change CPU/TCB assembly
layouts or solve arbitrary simultaneous multithreaded exec.

Use the native thread API or an equivalent CLONE_SETTLS-aware runtime for TLS
workers. Legacy message/GUI callback thread entry paths do not create EPF TLS
blocks and are outside this runtime contract. C/C++ constant-initialized POD
TLS is covered. C++ dynamic TLS initialization and destruction use the ABI 3
contract below; shared XAPI/libc integration remains later work.

## C++ thread-local initialization and destruction

Compiler-generated namespace and function-local `thread_local` initialization
uses the compiler's access hooks and guards in each thread. A compiler may
initialize a group of namespace TLS objects from one translation unit together;
function-local guards preserve their own first-access behavior. Guards and
objects live in the thread's existing TLS block; the loader does not call one
thread's initializer for another thread. The example uses real G++/Clang output, aligned objects,
main and shared-library storage, and constructors with per-thread input.

`libepf-runtime.eps` exports the three-argument C ABI functions
`__cxa_thread_atexit` and `__cxa_thread_atexit_impl`. They record a destructor,
object and image-local DSO handle in the current FS control block. Link one
`out/epf_rtld/cxx_abi.c.o` into each C++ executable and library; it defines a
hidden, distinct `__dso_handle`. Modules stay mapped throughout this process
lifetime, including all callbacks; there is no dlclose/unload protocol.

Registration returns zero, or minus one for a null destructor, allocation
failure or the per-thread limit. Records use private RW/NX anonymous pages,
up to 64 pages and 169 entries per page on x86-64. Failure leaves previous
registrations intact. Neither the interpreter nor a DSO owns a separate
global callback list: both runtime copies use FS offset 56, preserving the
existing 64-byte control-block size and canary offset.

Callbacks run in reverse registration order while the owning thread's FS and
TLS remain installed. The entry is removed before calling user code, so a
destructor may initialize another `thread_local`; the new destructor runs
before older pending callbacks. Empty pages are released during draining.
An initialization path that is never triggered registers no destructor.

A worker's normal callback return drains its TLS destructors before publishing
the result/completion and issuing thread exit. Therefore successful join sees
destructor effects before reclaiming the TLS and stack. Main's EPF CRT return
drains main-thread TLS before any module FINI, then runs reverse module FINI
order. Callbacks first registered by FINI are drained once FINI completes.
Repeated draining of an empty list does nothing.

Raw thread/process exit syscalls, fatal signals and forced termination bypass
this normal-return contract. After a worker becomes quiescent, its joiner
discards leftover callback pages without executing them with the joiner's FS.
This extends the existing `-ECANCELED` join behavior. It does not provide
exception unwinding, cancellation cleanup, a complete C++ standard library or
global-object `__cxa_atexit` services. Compile these freestanding examples with
`-fno-exceptions -fno-rtti` and the EPF-aware CRT.

The interface shape and lifetime ordering follow the
[LLVM libc++abi thread-exit contract](https://llvm.googlesource.com/libcxxabi/+/refs/heads/master/src/cxa_thread_atexit.cpp)
and [Itanium ABI DSO identity rules](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#dso-dtor).
The EPF implementation is first-party C and adds no linked third-party runtime.

## Limits and errors

| Resource | Limit |
|---|---:|
| File bytes per image | 64 MiB |
| Kernel EXEC reservation | 1 GiB |
| User-linker SHARED reservation | 64 MiB |
| LOAD alignment for interpreted images / libraries | 2 MiB |
| Segments / optional sections | 256 / 4096 |
| Directory entries / relocations per image | 256 / 65536 |
| TLS storage per module / aggregate per thread | 1 MiB / 16 MiB |
| TLS alignment | Up to 2 MiB |
| Pending TLS destructor pages / entries per thread | 64 / 10816 |
| Dynamic symbols / string bytes per image | 65536 / 4 MiB |
| Modules including main / NEEDED per module | 32 / 32 |
| INIT or FINI entries per module | 1024 |
| User-linker allocations, including snapshots and reservations | 256 MiB |
| Metadata character/lookup/range work units | 100 million |

Kernel malformed-image errors use `-ENOEXEC`; unsupported capabilities use
`-EOPNOTSUPP`, reservation collisions `-EEXIST`, allocation failures `-ENOMEM`,
missing interpreter `-ENOENT`, and oversized files `-EFBIG`. User-linker errors
emit `ld-epf:` diagnostics and exit 127. Its mappings are process-owned and are
reclaimed on exit. Shared code/data are currently private copies per process.

Exception-unwind registration, symbol versions, IFUNC, COPY, instruction
bit-field relocations, lazy binding, dlopen/dlclose and shared XAPI/libc
packaging, global C++ atexit services, and thread cancellation
APIs remain unsupported. `.epo`/REL static linking is a separate toolchain
component. SHARED and REL are not process entry files. Only format version 1.0
is accepted until explicit minor-version compatibility is implemented.

## Producing files and image integration

Both build graphs expose these targets:

```sh
./ninja -f build.ninja epf                  # out/shell.epf, static companion
./ninja -f build.ninja out/calc.epf         # product checkout only
./ninja -f build.ninja epf_rtld             # runtime and both dynamic examples
./ninja -f build.ninja epf_tls              # TLS runtime, libraries and thread example
./ninja -f build.ninja epf_cxx_tls          # C++ TLS construction/destruction example
./ninja -f build.ninja all
```

`all` includes `out/ld-epf-x86_64.epf`, `out/libepf-demo.eps` and
`out/epf-dynamic-demo.epf`, `out/epf-tls-demo.epf`, `out/epf-cxx-tls-demo.epf`
and their libraries, including `out/libepf-cxx-tls.eps`.
Both image staging paths install the interpreter at
`/system/lib/ld-epf-x86_64.epf`, the library under `/system/lib/epf/`, and the
examples/documentation under `/apps/epf-dynamic-demo/`. The native header is
installed as `/system/include/epf_runtime.h`. The product installer
uses the same staging function. Existing application paths keep their ELF
outputs. Image rebuilds do not migrate an installed system automatically.

The standalone Python standard-library producer is
`OpenXJ380/tools/elf_to_epf.py` in the parent workspace (`tools/elf_to_epf.py`
in the public checkout):

```sh
python3 OpenXJ380/tools/elf_to_epf.py input.elf --output output.epf
python3 OpenXJ380/tools/elf_to_epf.py library.so --shared --output library.eps
python3 OpenXJ380/tools/elf_to_epf.py program.elf --output program.epf \
    --interpreter /system/lib/ld-epf-x86_64.epf
```

The producer translates PT_TLS, STT_TLS and x86-64 DTPMOD64/DTPOFF64/TPOFF64
records, and selects native ABI 2 when TLS is used. `--require-tls` declares
TLS ABI use in hand-written assembly or runtime code without compiler TLS
metadata. Thread-exit ABI symbols select ABI 3 automatically, even without
compiler TLS symbols. `--abi-version 3` marks the current interpreter's kernel
requirement; forcing an ABI below an input's minimum is rejected. Source PT_TLS
starts must meet their TLS
alignment. Link compiler TLS code against the ELF intermediate
`out/epf_rtld/runtime.elf`, then convert the finished program/library; `.eps`
is the installed runtime format, not an input to the host ELF linker.

Inputs must already implement the native entry/function/syscall ABI. This tool
repackages linked ELF64 x86-64 EXEC/DYN code; it does not translate Linux ABIs,
compile source or link REL objects. It pads LOAD prefixes for EPF alignment,
preserves code/data/BSS addresses, emits independent read-only EPF metadata,
and omits section tables. Static mode accepts NONE and symbol-free ELF
RELATIVE/RELATIVE64. Dynamic mode uses SysV DT_HASH to bound the source ELF
symbol table (link with `--hash-style=both` or `sysv`), translates x86-64
64/32/PC32/GLOB_DAT/JUMP_SLOT/RELATIVE/RELATIVE64 into EPF scalar records, and
converts local ELF lifecycle pointers into RVA arrays. ELF INTERP must match
the explicitly requested native path. Unsupported semantics fail conversion.

Resolved fixed static GNU_RELRO becomes ordinary read-only LOADs. PIE, dynamic
or relocation-bearing RELRO emits native RELRO descriptors instead. Supported
ranges end on complete pages; leading page prefixes are protected too. Static
PIE no longer requires `-z norelro`. The EPF runtime and example use freestanding
PIC C, separate code/data pages, eager binding, a nonexecutable stack and RELRO;
see `tools/epf_rtld.py` for the complete build recipe.

The CLI bounds both stat size and actual reads. It writes through a temporary
file and atomic replacement, preserves an existing output on failure, and
rejects identical input/output paths.

## Verification

From the parent workspace:

```sh
python3 -m unittest discover -s tests -p 'test_elf_loader.py' -v
python3 -m unittest discover -s tests -p 'test_epf_pages.py' -v
python3 -m unittest discover -s tests -p 'test_epf_tool.py' -v
python3 -m unittest discover -s tests -p 'test_epf_rtld.py' -v
python3 -m unittest discover -s tests -p 'test_native_rtld.py' -v
python3 -m unittest discover -s tests -p 'test_epf_tls_kernel.py' -v
python3 -m unittest discover -s tests -p 'test_epf_cxx_tls.py' -v
python3 -m unittest discover -s tests -p 'test_epf_memory_operations.py' -v
python3 -m unittest discover -s tests -p 'test_epf_lazy_faults.py' -v
python3 -m unittest discover -s tests -p 'test_thread_preparation.py' -v
python3 tests/run_epf_qemu.py --image XJ380.img
python3 tests/run_epf_dynamic_qemu.py --image XJ380.img
python3 tests/run_epf_dynamic_qemu.py --tls --image XJ380.img
python3 tests/run_epf_dynamic_qemu.py --cxx-tls --image XJ380.img
```

Production kernel loader/page tests use ASan/UBSan, explicit target mappings,
simulated preemption and fault injection. They check ELF/static EPF behavior,
INTERP ownership, immutable handoff, static runtime RELRO, malformed data and
whole-chain mapping/VMA/allocation/protection rollback. Producer tests cover
layout, relocation translation, runtime RELRO, CLI failures and both graphs.

User-linker tests execute the actual production interpreter with real compiled
EPF inputs on Linux. A small host launcher substitutes only the initial kernel
mappings/stack; parsing, dependency loading, resolution, relocation, protection,
CRT and callbacks remain production code. Tests cover breadth-first binding,
local definitions, dependency-closure scope, SONAME identity, strong/weak rules,
cycles before callbacks, layer ordering/reverse finalization, all scalar types,
integer overflow, overlapping writes, malformed metadata and concurrent private
instances. Linux supplies the syscalls and memory protection for these tests.

QEMU uses private image clones and production generated artifacts. Dynamic
coverage includes concurrent EPF processes, ELF-to-dynamic-EPF exec, empty argv,
missing-library status 127, constructors/finalizers, BSS/data isolation, actual
RELRO user-copy rejection, failed kernel exec preserving the old image, and
static ELF/static EPF/native dynamic ELF regression. Durable result files
remain authoritative if bounded serial diagnostics drop records. The canonical
image is never modified by these runners; host quit is not a guest shutdown test.

TLS coverage uses real GD, LD, IE and LE compiler output, C++ thread_local POD,
zero-file TLS with large alignment, relocated template pointers, malformed
TLS metadata, independent control blocks without compiler TLS, and repeated
thread create/join. QEMU runs two TLS processes concurrently, with eight joined
workers per process, and verifies main/worker initialization, zero fill,
private addresses and data isolation across scheduling. Kernel tests also
fault-inject clone preparation and validate deferred TID clearing against CPU
references, active preparation tickets and collection claims.

C++ TLS tests additionally check lazy/unused initialization, main and DSO
constructors, exactly-once LIFO destruction before join and module FINI,
registration from a running destructor, callbacks created by FINI, ABI 3
selection/downgrade rejection, and forced-worker exit without parent-side
destruction. A libc-free host harness runs the production TLS implementation
with mmap fault injection, page-boundary reentrancy, the pending-record limit
and complete callback-page reclamation. Actual mmap/munmap syscall tests use
ASan/UBSan with concurrent callers, hint collisions and allocation/unmap failure
injection, checking that selection and teardown hold the same preemptible gate.
Production lazy-fault tests cover concurrent/repeated faults, recursive VM
entry, reservation splits, allocation/queue failures and zero-before-PTE
publication, with separate checks of exception IRQ/preemption scopes.
`--cxx-tls` runs two concurrent EPF
processes with eight C++ workers each, exec/empty argv/missing-library checks
and the existing ELF/static EPF/native dynamic ELF regressions.
