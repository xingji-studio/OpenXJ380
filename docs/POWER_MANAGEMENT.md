# Power actions and S5 shutdown

The product logo menu calls `OpenXJ380Socket_PowerAction`, which dispatches to
the kernel power driver. The loader supplies the physical RSDP in an optional
`BOOT_CONFIG` tail guarded by `BOOT_FLAG_ACPI_ROOT`. All previous prefix/tail
offsets remain unchanged. Both kernel entries copy the new field only when
the flag is present, so older loaders remain usable.

uACPI 7.0.0 loads the DSDT/SSDT namespace in a BSP task after normal scheduling
starts. EC handlers are installed before namespace `_REG`/`_INI` processing.
Separate BSP workers deliver SCI by polling and run deferred work. This
preserves the existing IRQ routing: its SCI input stays masked. The normal
shutdown sequence is:

1. Evaluate S5 sleep types and call uACPI's S5 preparation, including optional
   `_PTS(5)` and `_SST`, exactly once with interrupts and scheduling enabled.
   Stop new EC queries and drain deferred work while SCI/global-lock completion
   is still available. A failed preparation prohibits direct S5 register writes;
   firmware shutdown remains available. A firmware AML `Fatal` is recorded
   explicitly because uACPI 7 ignores the request callback's return value.
2. Finish filesystem work while task scheduling and storage I/O are available.
3. Disarm standard PCI PME wake on enumerated network and USB functions using
   ECAM. Preserve their current D state, DMA configuration and other PMCSR bits.
4. Validate the FADT checksum, length and every register description before
   accessing ACPI power hardware. Later FADT members are optional; the existing
   ACPI 1.0 prefix retains its layout.
5. If necessary, request ACPI mode and poll SCI_EN for at most three seconds.
   An already-enabled platform receives no redundant SMI command. Issue mode
   ownership commands only from the BSP, as required by ACPI. Other CPUs can
   use an already-enabled mode or retain the firmware shutdown path.
6. Disable supported fixed RTC wake and PCI Express wake, then acknowledge only
   observed wake status bits. PCIEXP_WAKE_DIS is active high. Preserve fixed
   power-button enables and all GPE enables. Without AML namespace information,
   the kernel cannot identify a power button's GPE routing. Pending GPE status
   is acknowledged only on fixed-feature power-button platforms.
7. Prefer UEFI `ResetSystem(EfiResetShutdown)`. Its x64 runtime-table slot is
   0x68; the four boot/kernel header copies assert the standard layout.
8. If firmware returns or is unavailable, attempt the fixed-register S5 path
   using the evaluated sleep types.
   Read-modify-write PM1 control preserves unrelated bits and SCI_EN while
   clearing write-only GBL_RLS and old sleep bits. Program both sleep types,
   flush caches, then assert SLP_EN. HW-reduced platforms use their byte-wide
   Sleep Control/Status GAS registers and never touch legacy PM1/GPE fields.
9. Apply fixed VM port fallbacks only to positively identified supported VMs.
   If all methods return, log the failure and halt with interrupts masked.

Firmware entry masks local interrupts while the temporary identity map is in
place. An unexpected return restores FPU state, the original mapping and the
original interrupt flags before logging or attempting other power methods.

## Supported boundaries

The complete uACPI interpreter is linked under MIT without modifying upstream
files. `third_party/uacpi/SOURCE.json` pins its commit, archive hash and every
imported file. Both Ninja generators retain the allocator wrappers and apply
uACPI's freestanding type override only to the ACPI sources. The integration
does not add another C runtime or linked dependency.

The EC driver supports byte-wide SystemIO/SystemMemory ECDT registers and two
one-byte IO resources from a `PNP0C09` device's `_CRS`. Read, write and query
commands use IBF/OBF handshakes with a 500ms bound per handshake and an additional
finite poll bound for a failed clock. EC accesses serialize and honor `_GLK`.
An AML Lock field may already own `_GL`; a small version-pinned C bridge checks
that ownership to avoid reacquiring it. The global lock always precedes the
controller mutex. EC query callbacks release both locks before evaluating
`_Qxx`, allowing that method to access EC fields. A query transport failure is
logged and stops repeated query delivery for that controller.

There is no S3/S4 resume implementation. `_TTS`, device `_DSW`/`_PSW`, GPIO and
GenericSerialBus operation-region drivers are outside this S5 integration.
The generic shutdown policy retains GPE enables; it does not derive a new S5
wake policy from device `_PRW` packages. Firmware remains the preferred final
power transition. Old loaders without RSDP retain the checksum-validated constant
`_S5_` fallback and explicitly log that AML preparation is unavailable. An AML
initialization or `_PTS` failure never uses this compatibility bypass.

The generic shutdown cleanup retains GPE enable bits even when FADT reports a fixed power button: a
namespace power-button device can override that indication. Disabling specific
GPE wake sources requires an evaluated `_PRW` routing map or target-machine
evidence. Do not infer EC commands or GPIO addresses from a ThinkPad model name.

AML PCI config and PCI wake preparation use ECAM; byte/word AML writes preserve
the exact access width and do not RMW the adjacent PCI status word.
PCI wake preparation is restricted to network/USB functions with
valid, bounded capability chains and a PM capability advertising PME support.
It skips legacy CF8/CFC operations because existing config access does not have
a common cross-CPU transaction lock. It does not stop or reset live controllers.
Adding driver quiesce/D3 transitions requires a separate ownership/worker-drain
protocol, especially when the root filesystem is on USB.

This S5 policy suppresses fixed RTC/PCIe and supported network/USB PME wake.
Firmware-controlled power-on on AC attach, EC behavior and unsupported generic
wake routing remain platform responsibilities until their interfaces are added.

## Validation

`python3 -m unittest discover -s tests -p test_power.py -v` executes the real
driver with privileged CPU instructions replaced by tracing adapters. It checks
the EFI x64 ABI in all four headers under both C and C++, malformed/short ACPI
tables, GAS priority and width, MMIO mapping, SCI handshake timeouts, W1C masks,
power-button/GPE preservation, S5 write ordering and bounded PCI capability walks.

`python3 -m unittest discover -s tests -p test_acpi.py -v` builds the actual
uACPI sources, production EC/runtime/host adapters, and compiler-generated ASL
fixtures under ASan/UBSan. It checks cross-SSDT `_PTS(5)`, ECDT and `_CRS`,
simultaneous AML Lock and `_GLK`, EC query-to-`_Qxx`, Fatal/timeout failure,
exactly-once preparation, source hashes, all four boot header layouts and both
actual kernel copy prologues against old buffers ending at previous tail bounds.
The checked-in `.aml` files record their iasl version and source/output hashes;
iasl is a separate host fixture compiler and is not linked into the kernel.

Use the project Ninja wrapper for the full build, then run both GUI paths:

```sh
./ninja -f build.ninja all -j6
python3 tests/run_power_qemu.py
python3 tests/run_power_qemu.py --force-acpi
python3 tests/run_power_qemu.py --force-acpi --smp 1
```

Run these commands from the parent XJ380 workspace. The runner requires an
existing GPT image with an ext4 root partition and ESP, `debugfs`, `mcopy`, and
QEMU with TCG. Use `--image` and `--qemu` to select their paths; `--kernel`
overrides the normal EFI test's kernel.
It clones the image, installs the built loader/kernel/modules into the clone,
logs in, launches two ordinary calculator windows, and clicks the logo-menu
shutdown action. It verifies namespace readiness and traces the actual uACPI
S5 preparation. `--force-acpi` links a
temporary test kernel with the EFI table unavailable and VM port alternatives
disabled; production objects and the base image remain untouched. Both paths require a QMP `guest-shutdown`
event and execution-trace evidence of the expected path. Results and logs
are saved under the printed `/tmp/xj380-power-qemu-*` directory.

QEMU validates register sequencing and the guest shutdown path. ThinkPad X1
Carbon 4th still needs tests with battery/AC and USB/LAN/dock combinations.
Capture its FADT/DSDT/SSDT and BIOS version before introducing a platform quirk.

## Specifications

- [UEFI runtime services table](https://uefi.org/specs/UEFI/2.11/04_EFI_System_Table.html#efi-runtime-services)
- [ACPI FADT and feature flags](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html)
- [ACPI fixed hardware and wake registers](https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html)
- [ACPI S5 transition and platform methods](https://uefi.org/specs/ACPI/6.6/16_Waking_and_Sleeping.html)
