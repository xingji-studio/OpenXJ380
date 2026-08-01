# BOOT KNOWLEDGE BASE

## OVERVIEW
UEFI bootloader subtree. Builds `out/BOOTX64.efi`, gathers firmware state, loads `\system\kernel.krl`, and passes `BOOT_CONFIG` into `KernelMain`.

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| UEFI entry | `bootx64.c` | `efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *)`. |
| EFI helpers | `bootlib.c`, `bootlib.h` | File, memory, ELF helper routines. |
| Boot headers | `include/` | Local EFI/ELF/ACPI/framebuffer definitions. |
| Boot LSP flags | `.clangd_template` | Separate include path: `${workspaceFolder}/boot/include`. |

## CONVENTIONS
- Built by root `Makefile` with `x86_64-w64-mingw32-gcc` and `BOOT_C_FLAGS`.
- Entry symbol is forced with `-e efi_main`; PE/COFF subsystem is set with `-Wl,--subsystem,10`.
- Boot code uses `-fshort-wchar`, `-nostdinc`, `-nostdlib`, and its own `boot/include` headers.
- Global EFI handles (`ST`, `BS`, `GOP`, `SFSP`, `LIP`, `SPP`, `BAT`, `IM`) are established in `bootx64.c`; preserve setup order.
- Boot menu options set `BOOT_FLAG_*` bits consumed later in `kernel/main.cpp`.
- Status/progress UI uses fixed small buffers; keep boot messages short.
- Keep boot-only helpers here. Kernel logic belongs in `kernel/`, not in the EFI loader.

## ANTI-PATTERNS
- Do not assume libc, heap, threads, or kernel facilities are available in boot code.
- Do not replace 16-bit EFI strings with normal `char *` paths where firmware APIs expect `CHAR16`/wide strings.
- Do not bypass `BOOT_CONFIG`; kernel-visible boot decisions should be passed through that struct/flags.
- Do not apply root `.clangd_template` to boot; it intentionally excludes `boot/.*`.

## BUILD / VERIFY
```bash
make all -j"$(nproc)"
make vdisk
```
Inspect `out/BOOTX64.efi`; runtime validation requires booting the image via `make run` or `make justrun`.
