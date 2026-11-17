# Power actions and S5 shutdown

The product logo menu calls `OpenXJ380Socket_PowerAction`, which dispatches to
the kernel power driver. The normal shutdown sequence is:

1. Finish filesystem work while task scheduling and storage I/O are available.
2. Disarm standard PCI PME wake on enumerated network and USB functions using
   ECAM. Preserve their current D state, DMA configuration and other PMCSR bits.
3. Validate the FADT checksum, length and every register description before
   accessing ACPI power hardware. Later FADT members are optional; the existing
   ACPI 1.0 prefix retains its layout.
4. If necessary, request ACPI mode and poll SCI_EN for at most three seconds.
   An already-enabled platform receives no redundant SMI command. Issue mode
   ownership commands only from the BSP, as required by ACPI. Other CPUs can
   use an already-enabled mode or retain the firmware shutdown path.
5. Disable supported fixed RTC wake and PCI Express wake, then acknowledge only
   observed wake status bits. PCIEXP_WAKE_DIS is active high. Preserve fixed
   power-button enables and all GPE enables. Without AML namespace information,
   the kernel cannot identify a power button's GPE routing. Pending GPE status
   is acknowledged only on fixed-feature power-button platforms.
6. Prefer UEFI `ResetSystem(EfiResetShutdown)`. Its x64 runtime-table slot is
   0x68; the four boot/kernel header copies assert the standard layout.
7. If firmware returns or is unavailable, attempt the fixed-register S5 path.
   Read-modify-write PM1 control preserves unrelated bits and SCI_EN while
   clearing write-only GBL_RLS and old sleep bits. Program both sleep types,
   flush caches, then assert SLP_EN. HW-reduced platforms use their byte-wide
   Sleep Control/Status GAS registers and never touch legacy PM1/GPE fields.
8. Apply fixed VM port fallbacks only to positively identified supported VMs.
   If all methods return, log the failure and halt with interrupts masked.

Firmware entry masks local interrupts while the temporary identity map is in
place. An unexpected return restores FPU state, the original mapping and the
original interrupt flags before logging or attempting other power methods.

## Supported boundaries

There is currently no AML namespace interpreter or ACPI EC driver in this
kernel. The fixed-register fallback accepts only a bounded constant `_S5_`
package in a checksum-validated DSDT. It does not execute `_PTS(5)`, `_TTS`,
device `_DSW`/`_PSW`, or EC operation regions. The runtime log identifies this
fallback explicitly. Firmware remains the preferred platform shutdown path.

GPE enable bits are retained even when FADT reports a fixed power button: a
namespace power-button device can override that indication. Disabling specific
GPE wake sources requires an evaluated `_PRW` routing map or target-machine
evidence. Do not infer EC commands or GPIO addresses from a ThinkPad model name.

PCI wake preparation is restricted to ECAM-backed network/USB functions with
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

Use the project Ninja wrapper for the full build, then run both GUI paths:

```sh
./ninja -f build.ninja all -j6
python3 tests/run_power_qemu.py
python3 tests/run_power_qemu.py --force-acpi
```

Run these commands from the parent XJ380 workspace. The runner requires an
existing GPT image with an ext4 root partition and ESP, `debugfs`, `mcopy`, and
QEMU with TCG. Use `--image` and `--qemu` to select their paths; `--kernel`
overrides the normal EFI test's kernel.
It clones the image, installs the built loader/kernel/modules into the clone,
logs in and clicks the logo-menu shutdown action. `--force-acpi` links a
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
