# Random sources and compatibility

The kernel owns random-source selection. Initial seeding and periodic reseeding
try **RDSEED -> virtio-rng -> RDRAND**, in that order. CPU instructions are CPUID
gated and have bounded retries. The PCI entropy driver supports modern and
legacy/transitional devices, including the prefetchable 64-bit BARs used by QEMU.
It is built into the kernel and initialized after PCI discovery in both product
and provider boot paths, including installer and safe modes. Boot posts requests
without waiting; secure readiness is announced again when a later caller first
collects a reliable seed. Device reads never wait while scheduling is disabled.

A successful source supplies a full 256-bit key and 96-bit nonce to an independent
ChaCha20 generator. Output generation rekeys after each block; disclosed output
never includes the material used as the next key. Source polling, clocks,
allocation and scheduler yields are outside the generator spin lock. Periodic
source failure preserves an already securely seeded generator. No clock-derived
seed is ever promoted to secure readiness.

## Interfaces

- `get_secure_random_bytes`: secure kernel bytes, with complete failure wiping.
  Authentication, RSA/PBKDF2 salts/nonces and EPF PIE placement use this interface.
- `get_random_bytes`: ordinary bytes, preferring the secure generator and otherwise
  using a separate clock-seeded compatibility generator. `AT_RANDOM` keeps this
  compatibility behavior so the initial installer/login process can still start.
  Low-entropy boot is visible as `rng: secure source=unavailable`.
- `get_identifier_random_bytes`: public GUIDs/UUIDs, preferring secure output and
  otherwise diversifying software output with clocks and a non-wrapping request
  sequence. UUID version/variant, nonzero and distinct checks remain in the
  installer. `RandomSource` reports `rdseed`, `virtio-rng`, `rdrand`, `software` or
  `unavailable`; the legacy `used_hardware` argument describes CPU entropy only.
- `get_hardware_random_bytes`: retained CPU-only primitive; authentication and
  ordinary consumers no longer call it directly.
- `kernel_random_u32`: protocol-lock-safe output for lwIP ports, DNS/DHCP IDs,
  randomized protocol timers and TCP initial sequence numbers. Boot initializes
  its fast generator before network modules start.

The existing Linux-numbered `getrandom` syscall (318) accepts `GRND_NONBLOCK=1`,
`GRND_RANDOM=2` and `GRND_INSECURE=4`. Unknown flags and RANDOM+INSECURE are rejected.
Normal/RANDOM requests require secure readiness. NONBLOCK never waits for device
DMA and returns EAGAIN before readiness. Normal requests use bounded source waits
(up to 100 ms for virtio) and return ENODATA if no secure seed can be obtained.
This bounded failure is intentional rather than Linux's indefinite initial-seed
wait. INSECURE explicitly permits ordinary compatibility bytes. Reads may be
short; the XAPI wrapper retries, and wipes the full caller buffer on eventual
failure. Zero-length requests succeed without dereferencing the buffer.

`xapi_secure_random` uses syscall 318, without user-mode CPU instructions.
`xapi_random_bytes` requests ordinary non-secret output. Account OAEP, native TLS,
legacy HTTPS and PSA callbacks use the secure interface. `getentropy` and
`arc4random` also require secure output; arc4random exits the process on source
failure because its API has no error return. `rand` and the browser's Math.random
binding use ordinary output. The browser override is first-party code; upstream
QuickJS and libwebp are unchanged. Libwebp dithering, CRC/hash seeds, fixed protocol
GUIDs and collision-checked recycle-bin names retain their algorithm semantics.

The installer applies the same policy to disk/partition GUIDs and ext4/FAT root
identities. The FatFs adapter replaces time-derived volume serials in both primary
and backup FAT32 BPBs for ordinary format and ESP creation, then syncs and reads
both back. The upstream FatFs GPT generator is not enabled (FF_LBA64=0).

## QEMU

The standard product and provider launchers include the built-in host RNG backend:

```text
-object rng-builtin,id=rng0 -device virtio-rng-pci,rng=rng0
```

This also works with Windows QEMU/WHPX; it does not require a host /dev/urandom
path. A current kernel and rebuilt SDK clients are required: adding the device cannot
fix an older guest that lacks its driver or clients that still execute CPU random
instructions directly. The existing Windows command can retain its memory,
firmware and raw disk arguments and add the two RNG options above. CPU random
instructions are optional when a working virtio-rng device is present.

Virtqueue completions are checked for index, descriptor ID and returned length.
Short completions are accumulated. Nonblocking reads can collect a pending request
later; timeouts do not free or reuse outstanding DMA memory. Queue/buffer pages
remain pinned until reboot, including failure after queue publication. Logs expose
source names and failure stages, never seed/key/random contents. A virtio host is
trusted to provide entropy; cloned VM execution state still needs a fresh host
source, and deterministic time is not claimed to be cryptographic entropy.

## Regression commands

```sh
PYTHONPATH=tests python3 -m unittest test_rng test_virtio_rng test_random_api test_auth test_xtls
PYTHONPATH=tests python3 -m unittest test_installer_native_service test_block_access_glue test_fatfs_guard_recovery test_installer_esp test_elf_loader
python3 tests/run_rng_qemu.py --image /path/to/disposable-fat32-base.img --gui-login
python3 tests/run_epf_qemu.py --image /path/to/disposable-fat32-base.img --cpu qemu64 --virtio-rng
python3 tests/run_xhttp_qemu.py --image /path/to/disposable-fat32-base.img --cpu qemu64,-rdseed,-rdrand --virtio-rng --memory-mib 4096 --timeout 600
```

The QEMU runner clones the supplied FAT test image; it never changes the source.
Its matrix covers modern/legacy virtio with both CPU instructions disabled,
RDRAND-only, RDSEED priority, and no secure source. Positive cases create a real
account through the production SDK and RSA/OAEP service; the negative case checks
rejection and output wiping. The optional GUI check cold-boots that disposable
account with the unmodified production login, and creates a first administrator
through the actual UI on a separate owned clone. VM termination is a host monitor
quit, not a claim of clean guest shutdown. Windows/WHPX must additionally be
verified on a Windows host.

The native TLS/HTTP runner also exercises shell, signal/libc/POSIX demos and GUI
process removal. Its local servers use the guest's 30-second operation deadline
to support software CPU emulation. Probe assertions are saved on the disposable
disk and checked again after the VM stops; lossy serial reaper messages are
diagnostic, while GUI process removal is checked through the production task API.
