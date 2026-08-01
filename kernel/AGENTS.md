# KERNEL KNOWLEDGE BASE

## OVERVIEW
Core kernel subtree. `KernelMain` owns boot-time initialization, CPU/memory/task/syscall setup, filesystem services, SMP barriers, and module loading policy.

## STRUCTURE
```
kernel/
├── main.cpp        # KernelMain init sequence
├── cpu/            # MSR/CPUID/FPU/FSGSBASE/register helpers
├── intr/           # Assembly interrupt/syscall handlers and ABI frame
├── memory/         # HHDM, frames, page tables, heap mapping
├── pctable/        # IDT/GDT/TSS/APIC table setup
├── smp/            # AP startup and per-CPU scheduler state
├── syscall/        # syscall MSR setup and dispatch
├── task/           # PCB/TCB, scheduler, IPC, reaper, poll/mutex
└── user/           # Kernel-side user process/app helpers
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Boot sequence | `main.cpp` | Exact init ordering and boot flags. |
| Kernel config | `build_settings.h`, generated `build_config.h` | Version, stack, heap, default app, feature toggles. |
| Interrupt ABI | `intr/handler.S`, `intr/handler.h` | Saved-register layout and return paths. |
| Scheduler | `task/scheduler.cpp`, `include/task/scheduler.h` | Per-CPU queues and context switch assumptions. |
| PCB/TCB layout | `include/task/pcb.h`, `task/pcb.cpp` | Process/thread ABI-like structures. |
| Dynamic linker/modules | `dlinker.cpp`, `include/dlinker.h` | Loads user shared objects and `.sys` modules. |

## CONVENTIONS
- Kernel sources are freestanding GNU++17/C11 and are linked with built-in `driver/`, `graphics/`, `font/`, and `lib/` objects into `out/kernel.krl`.
- `KernelMain` starts with interrupts and scheduler disabled; many initialization phases depend on that state.
- Early order matters: CPU → IDT/GDT → timers/APIC → HHDM/frame/heap → device/VFS/storage → process/SMP → rootfs/syscalls/modules → scheduler open.
- `open_interrupt` / `close_interrupt`, `enable_scheduler()` / `disable_scheduler()`, and `no_interrupt` are stateful gates; do not move them casually.
- Per-CPU setup happens both on BSP and AP paths. GDT/TSS, syscall MSRs, LAPIC, idle threads, and scheduler queues must stay consistent.
- `kernel/build_settings.h` is a manual source of truth for runtime defaults; the generated Ninja graph writes `kernel/build_config.h`.
- Boot flags can skip module loading and force safer AHCI/storage behavior.

## ANTI-PATTERNS
- Do not change `intr/handler.S` frame layout without auditing scheduler/context-switch/syscall users.
- Do not add mandatory boot functionality as a loadable module; mandatory code belongs under `kernel/` or built-in `driver/`.
- Do not assume paging, heap, VFS, current task, or scheduler exists before its `KernelMain` phase.
- Do not treat `kernel/dlinker.cpp` as module-only; it also handles user shared-object loading.
- Do not reorder SMP readiness waits unless you understand `scheduler_is_ready == xsi->cpu_count`.

## VERIFY
Run at least `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja all` after kernel changes. Boot-visible changes need `ninja -f build.ninja run`/QEMU and `serial.log` inspection.
