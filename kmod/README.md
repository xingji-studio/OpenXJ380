# Common kernel modules

`kmod/` owns the common loadable drivers and networking service used by both
standalone OpenXJ380 and the parent XJ380 product. Each module is an ELF shared
object with a `dlmain` entrypoint, built as `out/*.sys` and installed into the
image's `/system/mod` directory.

| Directory | Role |
| --- | --- |
| `e1000/` | Intel E1000 NIC, including multicast reception for IPv6 |
| `rtl8139/` | Realtek RTL8139 NIC |
| `virtio-net/` | Legacy/transitional VirtIO NIC |
| `netserver/` | lwIP OS port, IPv4/IPv6, sockets, native network provider |
| `xhci/` | xHCI controller and USB core/hub/storage support |
| `dlinker_test/` | Dynamic-linker fixtures, not built by default |

`tools/kernel_modules.py` is the shared source manifest and compile/link rule
emitter. Both Ninja generators consume it, and both image builders use its
module artifact list. Common modules compile against OpenXJ380's ABI headers.
Product graphics remain in the parent's `kmod/graphics/` and are absent from
the standalone build.

Generate the standalone graph in this repository and build its `kmods` target.
When working inside the parent checkout, use the parent's guarded Ninja entry:

```sh
(cd OpenXJ380 && python3 tools/gen_ninja.py --out build.ninja)
./ninja -C OpenXJ380 -f build.ninja kmods
```

`BUILTIN_XHCI=1` is the default: xHCI is linked into the kernel, and staging
removes a stale `xhci.sys` from `/system/mod`. `BUILTIN_XHCI=0` stages the module
instead. Set the same value for generation, build, and staging. The module
artifact is always built with separate PIC objects, never with built-in flags.
Staging also removes legacy `.km` files for the managed modules.

The unified netserver preserves the parent's IPv6 and native provider support,
log policy, OS-port waits, and socket options. Its lwIP vendor tree remains the
provider's existing copy; project policy lives in `netserver.cpp`, `arch/`, and
the first-party `lwipopts.h` override. The exact compiled lwIP subset and its
redistribution notices are maintained together by the shared manifest and
`tools/generate_lwip_notice.py`.

Keep mandatory core code in `kernel/`, built-in hardware drivers in `driver/`,
and common loadable modules here. The sound drivers remain built in.

## Migration validation

The parent test `tests/test_kmod_unification.py` checks both repositories and
both xHCI modes: common sources and link inputs match, graphics stay outside
the standalone graph, and staging replaces managed legacy `.km` files.

The parent's `tests/run_xnet_qemu.py` can boot a separate FAT32 fixture with
`--image`, select `--nic e1000|rtl8139|virtio-net-pci`, and use standalone
artifacts with `--artifacts OpenXJ380/out --entry shell`. Select the matching
`--builtin-xhci 0|1`; `--usb` also attaches an xHCI controller and USB mouse.
The probe checks TCP/UDP exchange with local host peers, socket state, network
configuration, permission checks, and handle reclamation. It changes a clone
and stops the guest through the host monitor.

The consolidation checks passed 35/35 runtime assertions for E1000 in the
parent's built-in and module xHCI modes and the standalone module xHCI mode.

RTL8139 now respects TSD ownership and transmission completion, pads short
Ethernet frames, and configures circular RX DMA consistently with its ring
reader. Its receive cursor wraps at 32 KiB, and the multicast filter admits
the IPv6 traffic required by netserver. Register semantics follow the
[Realtek RTL8139C(L) datasheet, sections 6.2 and 6.8](https://pdf.dzsc.com/RTL/RTL8139.pdf).

The legacy VirtIO NIC allocates each queue at its advertised size, uses the
10-byte header for its negotiated features, and notifies RX after DRIVER_OK.
TX buffers remain owned by the device until their used-ring completion is
reaped; submitting a packet does not wait for hardware while holding the queue
lock. Initialization failures detach configured queues before freeing DMA.
These layouts follow the [VirtIO specification, sections 2.7.2, 4.1.5.1.3.1,
and 5.1.6.1](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html).

`tests/test_nic_drivers.py` executes the actual driver functions with bounded
register/DMA adapters under ASan/UBSan. It covers queue sizes 16/128/256/1024,
asymmetric RX/TX sizes, allocation failure, full TX queues, out-of-order
completion, invalid RX metadata, and complete 16-bit index wrap.

Use `--stress` in the parent's QEMU runner to exchange 512 KiB TCP data and
384 1200-byte UDP datagrams with host peers that verify every byte. The five
cases below pass all 37 runtime assertions each:

| Kernel | NIC | RX/TX queue configuration |
| --- | --- | --- |
| XJ380 parent | RTL8139 | 32 KiB RX ring / 4 TX buffers |
| XJ380 parent | VirtIO | Default 256/256 |
| XJ380 parent | VirtIO legacy | 512/256 (`--virtio-rx-queue-size 512`) |
| Standalone OpenXJ380 | RTL8139 | 32 KiB RX ring / 4 TX buffers |
| Standalone OpenXJ380 | VirtIO | Default 256/256 |

These QEMU 6.2 cases use four virtual CPUs and an attached USB mouse/xHCI
controller, with xHCI built in. They validate emulator behavior; physical NIC
hardware has not been exercised by this suite. The sound drivers are unchanged.
