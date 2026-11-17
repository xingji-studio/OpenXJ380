"""Common module sources, build rules, and image artifacts for both products.

Paths are relative to the selected OpenXJ380 checkout, while outputs stay in
the caller's out/ directory. Product modules such as graphics are added by the
parent build and never become dependencies of the standalone kernel.
"""
from pathlib import Path


DRIVER_SOURCES = {
    "e1000": ("e1000.cpp",),
    "rtl8139": ("rtl8139.cpp",),
    "virtio-net": ("virtio_net.cpp",),
}
XHCI_SOURCES = ("usb_core.cpp", "usb_hub.cpp", "usb_msc.cpp", "xhci.cpp")

# Keep this explicit: unused lwIP subsystems can require unsupported OS hooks.
# The redistribution notice generator consumes this same compiled-source list.
LWIP_C_SOURCES = (
    "lwip/core/def.c",
    "lwip/core/dns.c",
    "lwip/core/init.c",
    "lwip/core/inet_chksum.c",
    "lwip/core/ip.c",
    "lwip/core/mem.c",
    "lwip/core/memp.c",
    "lwip/core/netif.c",
    "lwip/core/pbuf.c",
    "lwip/core/stats.c",
    "lwip/core/sys.c",
    "lwip/core/tcp.c",
    "lwip/core/tcp_in.c",
    "lwip/core/tcp_out.c",
    "lwip/core/udp.c",
    "lwip/core/timeouts.c",
    "lwip/core/ipv4/dhcp.c",
    "lwip/core/ipv4/etharp.c",
    "lwip/core/ipv4/icmp.c",
    "lwip/core/ipv4/ip4.c",
    "lwip/core/ipv4/ip4_addr.c",
    "lwip/core/ipv6/icmp6.c",
    "lwip/core/ipv6/ethip6.c",
    "lwip/core/ipv6/ip6.c",
    "lwip/core/ipv6/ip6_addr.c",
    "lwip/core/ipv6/nd6.c",
    "lwip/core/ipv6/mld6.c",
    "lwip/netif/ethernet.c",
    "lwip/api/api_lib.c",
    "lwip/api/api_msg.c",
    "lwip/api/err.c",
    "lwip/api/netbuf.c",
    "lwip/api/netifapi.c",
    "lwip/api/tcpip.c",
)
NETSERVER_CXX_SOURCES = ("netserver.cpp", "arch/sys_arch.cpp", "arch/utils.cpp")
MODULE_NAMES = (*DRIVER_SOURCES, "netserver", "xhci")


def module_artifacts(builtin_xhci: bool) -> list[str]:
    """Return only the modules that should be loaded for this kernel mode."""
    return [f"{name}.sys" for name in MODULE_NAMES if name != "xhci" or not builtin_xhci]


def emit_rules(n, base: str, describe) -> None:
    """Use the provider ABI for common modules, including in the parent build."""
    provider = Path(base)
    includes = f"-I./{provider / 'include'}"
    net_includes = f"{includes} -I./{provider / 'kmod/netserver'} -I./{provider / 'kmod/netserver/lwip/include'}"
    n.var(
        "KMOD_CXXFLAGS",
        "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding "
        "-fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -fno-rtti "
        "-std=gnu++17 -fshort-wchar -nostdinc -fno-use-cxa-atexit -fno-threadsafe-statics -mno-80387 "
        "-Wno-int-to-pointer-cast -Wno-macro-redefined -Wno-c11-extensions -Wno-c99-extensions "
        "-Wno-gnu-statement-expression-from-macro-expansion -Wno-writable-strings -Wno-c++11-narrowing "
        "-fPIC -fvisibility=hidden -MMD -MP",
    )
    n.var(
        "NETSERVER_CFLAGS",
        "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding "
        "-fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -std=gnu11 "
        "-fshort-wchar -nostdinc -mno-80387 -fPIC -fvisibility=hidden -MMD -MP " + net_includes,
    )
    n.var("NETSERVER_CXXFLAGS", "$KMOD_CXXFLAGS " + net_includes)
    n.var("kmod_cxxflags", "$KMOD_CXXFLAGS")
    n.var("netserver_cflags", "$NETSERVER_CFLAGS")
    n.var("netserver_cxxflags", "$NETSERVER_CXXFLAGS")
    n.rule(
        "kmod_cxx",
        "mkdir -p $$(dirname $out) && $cxx $kmod_cxxflags " + includes + " -MF $out.d -c $in -o $out",
        describe("CXX", "$in"), depfile="$out.d",
    )
    n.rule(
        "netserver_cc",
        "mkdir -p $$(dirname $out) && $cc $netserver_cflags -MF $out.d -c $in -o $out",
        describe("CC", "$in"), depfile="$out.d",
    )
    n.rule(
        "netserver_cxx",
        "mkdir -p $$(dirname $out) && $cxx $netserver_cxxflags -MF $out.d -c $in -o $out",
        describe("CXX", "$in"), depfile="$out.d",
    )
    n.rule("kmod_link", "$cxx -shared $in $ldflags -o $out", describe("LD", "$out"))


def emit(n, base: str = ".") -> list[Path]:
    """Emit the same common module graph in either checkout's output tree."""
    source_root = Path(base) / "kmod"
    targets = []
    entry_flags = "-nostdlib -Wl,-e,dlmain"
    n.comment("COMMON KERNEL MODULES - OpenXJ380 sources and ABI")
    for name, sources in DRIVER_SOURCES.items():
        objects = []
        for source in sources:
            obj = Path("out/kmod") / name / Path(source).with_suffix(".o")
            n.build(obj, "kmod_cxx", source_root / name / source)
            objects.append(obj)
        target = Path("out") / f"{name}.sys"
        n.build(target, "kmod_link", objects, variables={"ldflags": entry_flags})
        targets.append(target)

    net_objects = []
    for sources, rule in ((LWIP_C_SOURCES, "netserver_cc"), (NETSERVER_CXX_SOURCES, "netserver_cxx")):
        for source in sources:
            obj = Path("out/kmod/netserver") / Path(source).with_suffix(".o")
            n.build(obj, rule, source_root / "netserver" / source)
            net_objects.append(obj)
    net_target = Path("out/netserver.sys")
    n.build(net_target, "kmod_link", net_objects, variables={"ldflags": entry_flags + " -Wl,-z,muldefs"})
    targets.append(net_target)

    # Separate PIC objects from the built-in xHCI objects. Always build the
    # artifact; image staging excludes it when the kernel already owns xHCI.
    xhci_objects = []
    for source in XHCI_SOURCES:
        obj = Path("out/kmod_module/xhci") / Path(source).with_suffix(".o")
        n.build(obj, "kmod_cxx", source_root / "xhci" / source)
        xhci_objects.append(obj)
    xhci_target = Path("out/xhci.sys")
    n.build(
        xhci_target, "kmod_link", xhci_objects,
        variables={"ldflags": entry_flags + " -Wl,-Bsymbolic -Wl,-z,muldefs"},
    )
    targets.append(xhci_target)
    return targets
