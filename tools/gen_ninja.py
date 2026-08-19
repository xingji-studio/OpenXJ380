#!/usr/bin/env python3
"""Generate the Python-backed Ninja graph for XJ380.

This file is intentionally more explicit than a normal packaged build project:
XJ380 is freestanding, mixes kernel/user/kmod/Rust outputs, and needs
stable staged artifact names under out/.  Keep build behavior in this generator
instead of hand-editing build.ninja, because build.ninja is regenerated.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build.ninja"

# Keep generated Ninja output easy to scan with colored progress labels.
LOG_RESET = "\033[0m"
LOG_COLORS = {
    "ASM": "\033[1;35m",
    "BOOT": "\033[1;35m",
    "CC": "\033[1;32m",
    "CHECK": "\033[1;34m",
    "CLEAN": "\033[1;31m",
    "CP": "\033[1;34m",
    "CXX": "\033[1;36m",
    "FETCH": "\033[1;34m",
    "FIND": "\033[1;34m",
    "FMT": "\033[1;34m",
    "GEN": "\033[1;35m",
    "GRAPH": "\033[1;34m",
    "HOSTLD": "\033[1;33m",
    "ISO": "\033[1;35m",
    "LD": "\033[1;33m",
    "OBJCOPY": "\033[1;35m",
    "PREP": "\033[1;34m",
    "RUN": "\033[1;32m",
    "RUST": "\033[1;32m",
    "SIZE": "\033[1;34m",
    "STAGE": "\033[1;34m",
    "VDISK": "\033[1;35m",
    "VMDK": "\033[1;35m",
}


def log_label(tag: str) -> str:
    """Return the colored `[TAG]` prefix used in Ninja descriptions."""
    return f"{LOG_COLORS[tag]}[{tag}]{LOG_RESET}"


def log_desc(tag: str, text: str = "") -> str:
    """Keep generated Ninja descriptions short but visually grouped."""
    return log_label(tag) if not text else f"{log_label(tag)} {text}"



def rel(path: Path | str) -> str:
    """Return a repository-relative path when the path lives under ROOT.

    Generated build files should be portable between Windows paths, WSL paths,
    and checked-out copies, so normal project inputs are emitted relative to the
    repository.  Absolute paths are kept only for external toolchain artifacts
    such as Rust sysroot libraries.
    """
    p = Path(path)
    if p.is_absolute():
        try:
            p = p.relative_to(ROOT)
        except ValueError:
            return p.as_posix()
    return p.as_posix()


def esc(path: str) -> str:
    """Escape characters that Ninja treats as syntax inside paths.

    Ninja uses `:` after output lists and `$` for continuations/variables, so
    Windows drive letters, spaces, and literal dollars must be escaped before a
    path can appear in a build edge.
    """
    return path.replace("$", "$$").replace(":", "$:").replace(" ", "$ ")


def paths(items: list[Path | str]) -> str:
    """Format a path list for a Ninja build edge.

    Callers pass raw Path/string objects; this helper centralizes the relpath
    conversion and Ninja escaping so every edge follows the same output style.
    """
    return " ".join(esc(rel(item)) for item in items)


def cmd_output(command: list[str]) -> str:
    """Run a small discovery command; missing tools produce an empty value.

    The generator should still be inspectable when optional host tools are not
    installed.  Hard requirements, such as the Rust target libraries, validate
    their own result after calling this helper.
    """
    try:
        return subprocess.check_output(command, cwd=str(ROOT), text=True, stderr=subprocess.DEVNULL).strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        return ""


def find_executable(names: list[str], extra_paths: list[Path] | None = None) -> str:
    """Find a tool while honoring common non-PATH locations used on WSL/Linux.

    Environment variables still win before this function is called.  The extra
    paths cover common Rust installations where `rustc` exists but the shell
    PATH inside the build environment has not been fully initialized.
    """
    extra_paths = extra_paths or []
    for name in names:
        found = shutil.which(name)
        if found:
            return found
        for base in extra_paths:
            candidate = base / name
            if candidate.exists() and os.access(candidate, os.X_OK):
                return str(candidate)
    return names[0] if names else ""


def find_rustc() -> str:
    """Resolve the Rust compiler used by no_std userland Rust apps."""
    override = os.environ.get("RUSTC")
    if override:
        return override
    return find_executable(
        ["rustc"],
        [
            Path.home() / ".cargo/bin",
            Path("/snap/bin"),
            Path("/home/leon/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin"),
        ],
    )


def rust_target_libs(rustc: str, target: str) -> list[Path]:
    """Resolve libcore/compiler_builtins for no_std Rust user apps at gen time."""
    libdir = os.environ.get("RUST_TARGET_LIBDIR", "")
    if not libdir:
        if not rustc:
            raise SystemExit("缺少 Rust 编译器：请安装 rustup/rustc，或设置 RUSTC=/path/to/rustc")
        libdir = cmd_output([rustc, "--print", "target-libdir", "--target", target])
        if not libdir:
            raise SystemExit(
                f"无法查询 Rust 目标库目录：{target}\n"
                f"请先运行：rustup target add {target}\n"
                "如果已经安装，请确认 RUSTC 指向同一个 rustup 工具链，然后运行：ninja reconfigure"
            )
    libs: list[Path] = []
    missing: list[str] = []
    for pattern in ("libcore-*.rlib", "libcompiler_builtins-*.rlib"):
        matches = sorted(Path(libdir).glob(pattern))
        if matches:
            libs.append(matches[-1])
        else:
            missing.append(pattern)
    if missing:
        raise SystemExit(
            f"Rust 目标库不完整：{target}\n"
            f"缺少：{', '.join(missing)}\n"
            f"目标库目录：{libdir}\n"
            f"请运行：rustup target add {target}\n"
            "然后运行：ninja reconfigure"
        )
    return libs


def opt_flag() -> str:
    """Translate OPT_LEVEL into the C/C++ optimization flag family."""
    return "-O" + os.environ.get("OPT_LEVEL", "2")


def rust_opt_flag() -> str:
    """Translate OPT_LEVEL into the rustc optimization flag family."""
    return "-C opt-level=" + os.environ.get("OPT_LEVEL", "2")


def c_diagnostic_color_flag() -> str:
    """Keep compiler color enabled unless the environment overrides it."""
    return os.environ.get("DIAGNOSTICS_COLOR") or "-fdiagnostics-color=always"


def rust_diagnostic_color_flag() -> str:
    """Keep rustc color enabled unless the environment overrides it."""
    return os.environ.get("RUST_DIAGNOSTICS_COLOR") or "--color always"


def find_files(base: str, suffixes: tuple[str, ...], *, max_depth: int | None = None) -> list[Path]:
    """Collect files with deterministic sorted output.

    The optional depth cap is used for vendored libraries where only the first
    layer belongs to the existing build contract.  Sorting keeps regenerated
    build.ninja diffs stable when filesystems enumerate entries differently.
    """
    root = ROOT / base
    if not root.exists():
        return []
    out: list[Path] = []
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix not in suffixes:
            continue
        if max_depth is not None:
            try:
                depth = len(path.relative_to(root).parts)
            except ValueError:
                continue
            if depth > max_depth:
                continue
        out.append(path)
    return sorted(out)


def headers(*bases: str) -> list[Path]:
    """Collect headers for implicit dependencies on generated object edges.

    These broad lists make initial dependency tracking conservative before
    compiler depfiles take over for subsequent builds.
    """
    out: list[Path] = []
    for base in bases:
        out.extend(find_files(base, (".h", ".hpp")))
    return sorted(out)


# Tiny writer for the subset of Ninja syntax this repository needs.
# It deliberately keeps commands as shell strings because the OS build still has
# many freestanding/linker steps that are clearer as explicit command lines.
class Ninja:
    def __init__(self) -> None:
        self.lines: list[str] = []
        self.default_order_only: list[Path | str] = []

    def line(self, text: str = "") -> None:
        self.lines.append(text)

    def comment(self, text: str) -> None:
        self.line(f"# {text}")

    def var(self, name: str, value: str) -> None:
        self.line(f"{name} = {value}")

    def var_list(self, name: str, values: list[Path | str]) -> None:
        """Emit long source lists over multiple Ninja lines."""
        escaped_values = [esc(rel(value)) for value in values]
        if not escaped_values:
            self.line(f"{name} =")
            return
        if len(escaped_values) == 1:
            self.var(name, escaped_values[0])
            return
        self.line(f"{name} = {escaped_values[0]} $")
        for value in escaped_values[1:-1]:
            self.line(f"  {value} $")
        self.line(f"  {escaped_values[-1]}")

    def rule(
        self,
        name: str,
        command: str,
        description: str | None = None,
        *,
        depfile: str | None = None,
        generator: bool = False,
        restat: bool = False,
    ) -> None:
        """Emit one Ninja rule and optional GCC-style depfile metadata."""
        self.line(f"rule {name}")
        self.line(f"  command = {command}")
        if description:
            self.line(f"  description = {description}")
        if depfile:
            self.line(f"  depfile = {depfile}")
            self.line("  deps = gcc")
        if generator:
            self.line("  generator = 1")
        if restat:
            self.line("  restat = 1")
        self.line()

    def build(
        self,
        outputs: list[Path | str] | Path | str,
        rule: str,
        inputs: list[Path | str] | Path | str | None = None,
        *,
        implicit: list[Path | str] | None = None,
        order_only: list[Path | str] | None = None,
        variables: dict[str, str] | None = None,
        use_default_order_only: bool = True,
    ) -> None:
        """Emit one Ninja build edge.

        `implicit` inputs appear after `|` and affect rebuild decisions without
        being passed to the command.  `order_only` inputs appear after `||` and
        enforce sequencing without making the output dirty when they change.
        Single values and lists are both accepted so call sites stay readable.
        """
        outs = [outputs] if isinstance(outputs, (str, Path)) else outputs
        ins: list[Path | str] = []
        if inputs is not None:
            ins = [inputs] if isinstance(inputs, (str, Path)) else inputs
        line = f"build {paths(list(outs))}: {rule}"
        if ins:
            line += f" {paths(ins)}"
        if implicit:
            line += f" | {paths(implicit)}"
        effective_order_only: list[Path | str] = []
        if use_default_order_only:
            effective_order_only.extend(self.default_order_only)
        if order_only:
            effective_order_only.extend(order_only)
        if effective_order_only:
            line += f" || {paths(effective_order_only)}"
        self.line(line)
        if variables:
            for key, value in variables.items():
                self.line(f"  {key} = {value}")
        self.line()


def root_objects(
    n: Ninja,
    nasm_files: list[Path],
    asm_files: list[Path],
    c_files: list[Path],
    cpp_files: list[Path],
) -> list[Path]:
    """Emit kernel/root object build edges and return their object paths."""
    # Root objects use the same source lists emitted in the generated FILES
    # section, so the readable list and the real build graph cannot drift.
    n.comment("ROOT OBJECTS - kernel, built-in drivers, console font, lib, optional built-in xhci")
    config = Path("kernel/build_config.h")
    settings = Path("kernel/build_settings.h")
    objs: list[Path] = []
    for src in nasm_files:
        obj = Path("out") / src.relative_to(ROOT).with_suffix(".o")
        n.build(obj, "nasm", src, implicit=[settings, config])
        objs.append(obj)
    for src in asm_files:
        obj = Path("out") / src.relative_to(ROOT).with_suffix(".o")
        n.build(obj, "root_as", src, implicit=[settings, config])
        objs.append(obj)
    for src in c_files:
        obj = Path("out") / src.relative_to(ROOT).with_suffix(".o")
        n.build(obj, "root_cc", src, implicit=[settings, config])
        objs.append(obj)
    for src in cpp_files:
        obj = Path("out") / src.relative_to(ROOT).with_suffix(".o")
        n.build(obj, "root_cxx", src, implicit=[settings, config])
        objs.append(obj)
    return objs


def xapi(n: Ninja) -> tuple[list[Path], Path, Path]:
    """Emit the minimal syscall and process-entry runtime."""
    n.comment("XAPI RUNTIME - declarations remain public; only syscall and entry shims are built")
    xapi_headers = headers("user/xapi/include")
    srcs = [
        ROOT / "user/xapi/arch/x86_64/crt0.S",
        ROOT / "user/xapi/libsys.cpp",
        ROOT / "user/xapi/xgui_stubs.cpp",
    ]
    core_objs: list[Path] = []
    for src in srcs:
        rel_src = src.relative_to(ROOT / "user/xapi")
        obj = Path("out/xapi") / Path(str(rel_src) + ".o")
        rule = "xapi_as" if src.suffix == ".S" else "xapi_cxx"
        n.build(obj, rule, src, implicit=xapi_headers)
        core_objs.append(obj)
    constart_obj = Path("out/xapi/constart.cpp.o")
    n.build(constart_obj, "xapi_headcon", "user/xapi/constart.cpp", implicit=xapi_headers)
    liballoc = Path("out/xapi/liballoc-x86_64.a")
    n.build(liballoc, "copy", "liballoc-x86_64.a")
    return core_objs, constart_obj, liballoc


def user_compile(n: Ninja, out: Path, src: str, flags: str, implicit: list[Path]) -> Path:
    # Most user apps compile one or more C++ objects and then link manually
    # against XAPI objects; using a custom cflags variable keeps rules reusable.
    n.build(out, "user_cxx_custom", src, implicit=implicit, variables={"cflags": flags})
    return out


def user_apps(n: Ninja, core_objs: list[Path], constart_obj: Path) -> list[Path]:
    """Emit the sole user-space example program."""
    n.comment("USER APPS - command-line example")
    shell_obj = Path("out/cli_shell.o")
    user_compile(n, shell_obj, "user/cli_shell.cpp", "$user_cflags", headers("user/xapi/include"))
    target = Path("out/shell.elf")
    n.build(target, "user_ld", core_objs + [constart_obj, shell_obj], variables={"message": log_desc("LD", target.as_posix())})
    return [target]


def kmods(n: Ninja) -> list[Path]:
    """Emit loadable kernel module artifacts."""
    # Kmods are shared objects with dlmain entrypoints.  Keep module compile
    # flags separate from root kernel flags because visibility/PIC differ.
    n.comment("KERNEL MODULES - loadable .sys outputs with dlmain entrypoints")
    targets: list[Path] = []
    # e1000 is first-party C++, so a shallow directory scan is enough and keeps
    # nested helper experiments from silently becoming part of the module ABI.
    n.comment("KMOD: e1000 PCI network driver")
    e1000_objs: list[Path] = []
    for src in find_files("kmod/e1000", (".cpp",), max_depth=1):
        obj = Path("out/kmod/e1000") / src.name.replace(".cpp", ".o")
        n.build(obj, "kmod_e1000_cxx", src)
        e1000_objs.append(obj)
    n.build("out/e1000.sys", "kmod_link", e1000_objs, variables={"ldflags": "-nostdlib -Wl,-e,dlmain"})
    targets.append(Path("out/e1000.sys"))

    # Netserver vendors lwIP, but this OS glue supports only the subset below.
    # Avoid enabling extra lwIP files accidentally because some depend on PPP or
    # platform hooks that are intentionally not wired in this tree.
    n.comment("KMOD: netserver lwIP subset and XJ380 arch glue")
    lwip_c = [
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
        "lwip/netif/ethernet.c",
        "lwip/api/api_lib.c",
        "lwip/api/api_msg.c",
        "lwip/api/err.c",
        "lwip/api/netbuf.c",
        "lwip/api/netifapi.c",
        "lwip/api/tcpip.c",
    ]
    net_objs: list[Path] = []
    for item in lwip_c:
        src = Path("kmod/netserver") / item
        obj = Path("out/kmod/netserver") / item.replace(".c", ".o")
        n.build(obj, "netserver_cc", src)
        net_objs.append(obj)
    for item in ("netserver.cpp", "arch/sys_arch.cpp"):
        src = Path("kmod/netserver") / item
        obj = Path("out/kmod/netserver") / item.replace(".cpp", ".o")
        n.build(obj, "netserver_cxx", src)
        net_objs.append(obj)
    n.build("out/netserver.sys", "kmod_link", net_objs, variables={"ldflags": "-nostdlib -Wl,-e,dlmain -Wl,-z,muldefs"})
    targets.append(Path("out/netserver.sys"))

    # xhci can be linked into the kernel when BUILTIN_XHCI=1, but the module
    # artifact is still generated for staging/testing parity.
    n.comment("KMOD: xhci USB stack module")
    xhci_objs: list[Path] = []
    for name in ("usb_core.cpp", "usb_hub.cpp", "usb_msc.cpp", "xhci.cpp"):
        src = Path("kmod/xhci") / name
        obj = Path("out/kmod_module/xhci") / name.replace(".cpp", ".o")
        n.build(obj, "kmod_xhci_cxx", src)
        xhci_objs.append(obj)
    n.build("out/xhci.sys", "kmod_link", xhci_objs, variables={"ldflags": "-nostdlib -Wl,-e,dlmain -Wl,-Bsymbolic -Wl,-z,muldefs"})
    targets.append(Path("out/xhci.sys"))
    return targets


def phony(n: Ninja, name: str, deps: list[Path | str]) -> None:
    """Emit compatibility targets such as `all`, `kmods`, and `build_xapi`."""
    n.build(name, "phony", deps)


def write_if_changed(path: Path, content: str) -> bool:
    """Write generated files only when bytes actually change.

    build.ninja is intentionally checked before every normal Ninja run.  If the
    generator rewrote identical content each time, Ninja would treat its own
    manifest as changed and restart repeatedly.
    """
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return False
    path.write_text(content, encoding="utf-8")
    return True


def main() -> None:
    # Generation is deterministic for a given checkout/environment.  Toolchain
    # probes happen here so generated flags and Rust library paths are visible
    # at the top of build.ninja.
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(OUT))
    parser.add_argument(
        "--fail-if-changed",
        action="store_true",
        help="write the manifest, then fail if it changed so Ninja can be rerun with the fresh graph",
    )
    args = parser.parse_args()

    # Environment knobs keep the same defaults as the historical build.
    builtin_xhci = os.environ.get("BUILTIN_XHCI", "0") == "1"
    cpp_extra = " -DXHCI_BUILTIN" if builtin_xhci else ""
    host_gcc = os.environ.get("HOST_GCC", "gcc")
    host_gcc_triple = cmd_output([host_gcc, "-dumpmachine"])
    rust_target = os.environ.get("RUST_TARGET", "x86_64-unknown-none")
    rustc = find_rustc()
    # No Rust user targets are currently emitted. Do not make C/C++ build graph
    # generation depend on an otherwise unused Rust target installation.
    rust_libs: list[Path] = []

    n = Ninja()
    n.comment("Generated by tools/gen_ninja.py. Run `ninja reconfigure` after changing the build graph.")
    n.comment("Edit tools/gen_ninja.py, not this generated build.ninja file.")
    n.var("ninja_required_version", "1.10")
    n.var("cc", os.environ.get("CC", os.environ.get("COMPILER_PREFIX", "") + "clang"))
    n.var("cxx", os.environ.get("CPP", os.environ.get("COMPILER_PREFIX", "") + "clang++"))
    n.var("nasm", os.environ.get("NASM", os.environ.get("COMPILER_PREFIX", "") + "nasm"))
    n.var("ld", os.environ.get("LD", os.environ.get("COMPILER_PREFIX", "") + "ld"))
    n.var("objc", os.environ.get("OBJC", os.environ.get("COMPILER_PREFIX", "") + "objcopy"))
    n.var("host_cc", os.environ.get("HOST_CC", "cc"))
    n.var("host_cxx", os.environ.get("HOST_CXX", "c++"))
    n.var("user_cc", os.environ.get("CC", os.environ.get("COMPILER_PREFIX", "") + "clang"))
    n.var("user_cxx", os.environ.get("CXX", "g++"))
    n.var("user_ld", os.environ.get("LD", "ld"))
    n.var("rustc", rustc)
    n.var("rust_target", rust_target)
    n.line()
    # The generated FILES section is intentionally verbose: it lets a developer
    # inspect exactly what the generator discovered without reading Python code.
    n.comment("FILES - generated source and header discovery lists")
    # These root lists intentionally feed both the readable FILES section and
    # root_objects(); one scan owns both display and build behavior.
    root_source_roots = ["kernel", "driver", "lib"]
    nasm_files: list[Path] = []
    asm_files: list[Path] = []
    c_files: list[Path] = []
    cpp_files: list[Path] = []
    for base in root_source_roots:
        nasm_files.extend(find_files(base, (".asm",)))
        asm_files.extend(find_files(base, (".S",)))
        c_files.extend(find_files(base, (".c",)))
        cpp_files.extend(find_files(base, (".cpp",)))
    if builtin_xhci:
        cpp_files.extend(find_files("kmod/xhci", (".cpp",)))
    xapi_sources = [ROOT / "user/xapi/arch/x86_64/crt0.S", ROOT / "user/xapi/libsys.cpp", ROOT / "user/xapi/xgui_stubs.cpp"]
    n.var("BUILTIN_XHCI", "1" if builtin_xhci else "0")
    n.var("ROOT_SOURCE_DIRS", " ".join(root_source_roots))
    n.var_list("NASM_FILES", nasm_files)
    n.var_list("ASM_FILES", asm_files)
    n.var_list("C_FILES", c_files)
    n.var_list("CPP_FILES", cpp_files)
    n.var_list("HEADER_FILES", headers("include"))
    n.var_list("XAPI_SOURCES", xapi_sources)
    n.var_list("XAPI_HEADERS", headers("user/xapi/include"))
    n.var_list("BOOT_HEADERS", find_files("boot/include", (".h", ".hpp")))
    n.var_list("KMOD_E1000_SOURCES", find_files("kmod/e1000", (".cpp",), max_depth=1))
    n.line()
    # Emit uppercase canonical flag variables first, then lowercase aliases for
    # older generated rule bodies.  That makes the build options readable while
    # preserving compatibility with existing variable names.
    n.comment("FLAGS - canonical compile and link options")
    n.var("OPT_LEVEL", os.environ.get("OPT_LEVEL", "2"))
    n.var("OPT_FLAG", "-O$OPT_LEVEL")
    n.var("RUST_OPT_FLAG", "-C opt-level=$OPT_LEVEL")
    n.var("DIAGNOSTIC_COLOR", c_diagnostic_color_flag())
    n.var("RUST_DIAGNOSTIC_COLOR", rust_diagnostic_color_flag())
    n.var("NASM_FLAGS", "-f elf64 -DX86_64_TARGET -DUEFI")
    n.var("ASM_FLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -mno-80387 -MMD -MP")
    n.var("C_FLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -std=c11 -fshort-wchar -nostdinc -mno-80387 -I./include -MMD -MP")
    n.var("CPP_FLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -fno-rtti -std=gnu++17 -fshort-wchar -nostdinc -fno-use-cxa-atexit -fno-threadsafe-statics -mno-80387 -Wno-int-to-pointer-cast -Wno-macro-redefined -Wno-c11-extensions -Wno-c99-extensions -Wno-gnu-statement-expression-from-macro-expansion -I./include -Wno-writable-strings -Wno-c++11-narrowing -MMD -MP" + cpp_extra)
    n.var("BOOT_C_FLAGS", "$DIAGNOSTIC_COLOR -g -I ./boot/include -Wextra -e efi_main -nostdinc -nostdlib -fno-builtin -Wl,--subsystem,10 -fshort-wchar")
    n.var("USER_CFLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -I ./include -Wno-write-strings -MMD -MP")
    n.var("RUST_FLAGS", "$RUST_DIAGNOSTIC_COLOR --edition=2024 --target $rust_target --emit=obj --crate-type lib -C panic=abort -C no-redzone=yes $RUST_OPT_FLAG -C debuginfo=2 -C overflow-checks=off")
    n.var("KMOD_CXXFLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -fno-rtti -std=gnu++17 -fshort-wchar -nostdinc -fno-use-cxa-atexit -fno-threadsafe-statics -mno-80387 -Wno-int-to-pointer-cast -Wno-macro-redefined -Wno-c11-extensions -Wno-c99-extensions -Wno-gnu-statement-expression-from-macro-expansion -Wno-writable-strings -Wno-c++11-narrowing -fPIC -fvisibility=hidden -MMD -MP")
    n.var("NETSERVER_CFLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -std=gnu11 -fshort-wchar -nostdinc -mno-80387 -fPIC -fvisibility=hidden -MMD -MP -I./include -I./kmod/netserver/lwip/include -I./kmod/netserver")
    n.var("NETSERVER_CXXFLAGS", "$KMOD_CXXFLAGS -MMD -MP -I./include -I./kmod/netserver/lwip/include -I./kmod/netserver")
    n.var("opt", "$OPT_FLAG")
    n.var("rust_opt", "$RUST_OPT_FLAG")
    n.var("diagnostic_color", "$DIAGNOSTIC_COLOR")
    n.var("rust_diagnostic_color", "$RUST_DIAGNOSTIC_COLOR")
    n.var("nasm_flags", "$NASM_FLAGS")
    n.var("asm_flags", "$ASM_FLAGS")
    n.var("c_flags", "$C_FLAGS")
    n.var("cpp_flags", "$CPP_FLAGS")
    n.var("boot_c_flags", "$BOOT_C_FLAGS")
    n.var("user_cflags", "$USER_CFLAGS")
    n.var("rust_user_flags", "$RUST_FLAGS")
    n.var("kmod_cxxflags", "$KMOD_CXXFLAGS")
    n.var("netserver_cflags", "$NETSERVER_CFLAGS")
    n.var("netserver_cxxflags", "$NETSERVER_CXXFLAGS")
    n.comment("Lowercase aliases above keep rule bodies compatible with older generated graphs.")
    n.var("host_gcc_triple", host_gcc_triple)
    n.line()

    n.comment("RULES - reusable shell commands used by build edges")
    # Rules stay generic; per-target differences are passed as edge variables so
    # command strings do not need to be duplicated for every app/module.
    n.rule(
        "regen",
        "python3 tools/gen_ninja.py --out build.ninja",
        log_desc("GEN", "build.ninja"),
        generator=True,
        restat=True,
    )
    n.rule(
        "refresh_manifest",
        "python3 tools/gen_ninja.py --out build.ninja --fail-if-changed && "
        "mkdir -p $$(dirname $out) && touch $out",
        log_desc("GEN", "refresh build.ninja"),
        generator=True,
        restat=True,
    )
    n.rule("nasm", "mkdir -p $$(dirname $out) && $nasm $nasm_flags $in -o $out", log_desc("ASM", "$in -> $out"))
    n.rule("root_as", "mkdir -p $$(dirname $out) && $cc -mcmodel=large $asm_flags -MF $out.d -c $in -o $out", log_desc("ASM", "$in -> $out"), depfile="$out.d")
    n.rule("root_cc", "mkdir -p $$(dirname $out) && $cc -fno-pic -fno-pie -mcmodel=large $c_flags -MF $out.d -c $in -o $out", log_desc("CC", "$in -> $out"), depfile="$out.d")
    n.rule("root_cxx", "mkdir -p $$(dirname $out) && $cxx -fno-pic -fno-pie -mcmodel=large $cpp_flags -MF $out.d -c $in -o $out", log_desc("CXX", "$in -> $out"), depfile="$out.d")
    n.rule("gen_config", "tmp=$out.tmp; { echo '#pragma once'; echo '#define CONFIG_KERNEL_BUILTIN_XHCI '$${BUILTIN_XHCI:-0}; } > $$tmp; if ! cmp -s $$tmp $out; then mv $$tmp $out; else rm -f $$tmp; fi", log_desc("GEN", "$out"))
    n.rule("boot", "mkdir -p $$(dirname $out) && x86_64-w64-mingw32-gcc $boot_c_flags -o $out boot/bootx64.c boot/bootlib.c", log_desc("BOOT", "$out"))
    n.rule("objcopy_bin", "mkdir -p $$(dirname $out) && $objc -I binary -O elf64-x86-64 $bin_input $out", log_desc("OBJCOPY", "$bin_input"))
    n.rule(
        "kernel_link",
        "$ld -z muldefs -T linker.ld --static --wrap=malloc --wrap=calloc --wrap=realloc --wrap=aligned_alloc "
        "-o $out $in",
        log_desc("LD", "$out"),
    )
    n.rule("copy", "mkdir -p $$(dirname $out) && cp -a $in $out", log_desc("CP", "$in -> $out"))
    n.rule("user_cxx_custom", "mkdir -p $$(dirname $out) && $user_cxx $cflags -MF $out.d -c $in -o $out", log_desc("CXX", "$in -> $out"), depfile="$out.d")
    n.rule("user_cc_custom", "mkdir -p $$(dirname $out) && $user_cc $cflags -MF $out.d -c $in -o $out", log_desc("CC", "$in -> $out"), depfile="$out.d")
    n.rule("rust_user", "mkdir -p $$(dirname $out) && $rustc $rust_user_flags $in -o $out", log_desc("RUST", "$in -> $out"))
    n.rule("xapi_cxx", "mkdir -p $$(dirname $out) && $user_cxx $diagnostic_color $opt -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -Wno-write-strings -MMD -MP -MF $out.d -c $in -o $out", log_desc("CXX", "$in"), depfile="$out.d")
    n.rule("xapi_as", "mkdir -p $$(dirname $out) && $user_cc $diagnostic_color $opt -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -Wno-write-strings -MMD -MP -MF $out.d -c $in -o $out", log_desc("ASM", "$in"), depfile="$out.d")
    n.rule("xapi_head", "mkdir -p $$(dirname $out) && $user_cc $diagnostic_color $opt -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -Wno-write-strings -MMD -MP -MF $out.d -c $in -o $out", log_desc("CXX", "start.cpp"), depfile="$out.d")
    n.rule("xapi_headcon", "mkdir -p $$(dirname $out) && $user_cc $diagnostic_color $opt -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -Wno-write-strings -MMD -MP -MF $out.d -c $in -o $out", log_desc("CXX", "constart.cpp"), depfile="$out.d")
    n.rule("user_ld", "$user_ld -Ttext=0x200000 $in -o $out", log_desc("LD", "$out"))
    n.rule("host_cc", "mkdir -p $$(dirname $out) && $user_cc $diagnostic_color -O0 -g -Wall -Wextra $in -o $out", log_desc("HOSTLD", "$in"))
    n.rule(
        "host_cxx_test",
        "mkdir -p $$(dirname $out) && $host_cxx -std=c++17 -O2 -Wall -Wextra $host_test_cxxflags "
        "-idirafter ./include $in -o $out.tmp && $out.tmp && mv $out.tmp $out",
        log_desc("CHECK", "$out"),
    )
    n.rule("mbedtls_cc", "mkdir -p $$(dirname $out) && $user_cc $mbedtls_ccflags -MF $out.d -c $in -o $out", log_desc("CC", "$in -> $out"), depfile="$out.d")
    n.rule("libvterm_cc", "mkdir -p $$(dirname $out) && $user_cc $libvterm_cflags -MF $out.d -c $in -o $out", log_desc("CC", "$in -> $out"), depfile="$out.d")
    n.rule("kmod_e1000_cxx", "mkdir -p $$(dirname $out) && $cxx $kmod_cxxflags -I./include -MF $out.d -c $in -o $out", log_desc("CXX", "$in"), depfile="$out.d")
    n.rule("kmod_xhci_cxx", "mkdir -p $$(dirname $out) && $cxx $kmod_cxxflags -I./include -I./kmod/xhci -MF $out.d -c $in -o $out", log_desc("CXX", "$in"), depfile="$out.d")
    n.rule("netserver_cc", "mkdir -p $$(dirname $out) && $cc $netserver_cflags -MF $out.d -c $in -o $out", log_desc("CC", "$in"), depfile="$out.d")
    n.rule("netserver_cxx", "mkdir -p $$(dirname $out) && $cxx $netserver_cxxflags -MF $out.d -c $in -o $out", log_desc("CXX", "$in"), depfile="$out.d")
    n.rule(
        "package_compliance",
        "python3 tools/package_third_party.py --output out/compliance/third-party",
        log_desc("STAGE", "third-party compliance"),
        restat=True,
    )
    n.rule("kmod_link", "$cxx -shared $in $ldflags -o $out", log_desc("LD", "$out"))
    n.rule("python_cmd", "python3 tools/ninja_build.py $cmd", "$desc")
    n.rule("shell_cmd", "$cmd", "$desc")

    n.comment("MANIFEST - regenerate build.ninja before every normal Ninja execution")
    manifest_inputs = [
        "LICENSES.md",
        "THIRD_PARTY_NOTICES.md",
        *[str(path) for path in sorted(Path("licenses").rglob("*")) if path.is_file()],
    ]
    # The manifest self-edge only tracks the generator itself.  The forced
    # preflight below checks hand-maintained graph-affecting inputs before every
    # normal build, without making source edits permanently dirty build.ninja.
    n.build(
        "build.ninja",
        "regen",
        ["tools/gen_ninja.py", "tools/ninja_build.py"],
        use_default_order_only=False,
    )
    # Ninja cannot safely force the build.ninja self-edge every run: that causes
    # manifest regeneration loops.  Instead, every real build edge below waits
    # for this forced preflight.  If the preflight changes build.ninja, it fails
    # before stale graph commands execute; the next `ninja` invocation reads the
    # refreshed graph.
    n.build("always_refresh_manifest", "phony", use_default_order_only=False)
    refresh_stamp = Path("out/.ninja-preflight")
    n.build(
        refresh_stamp,
        "refresh_manifest",
        ["tools/gen_ninja.py", "tools/ninja_build.py", "always_refresh_manifest"],
        implicit=manifest_inputs,
        use_default_order_only=False,
    )
    n.default_order_only = [refresh_stamp]
    n.comment("CORE ARTIFACTS - bootloader, config header, support library, kernel")
    n.build("kernel/build_config.h", "gen_config")

    kernel_objs = root_objects(n, nasm_files, asm_files, c_files, cpp_files)
    n.build("out/BOOTX64.efi", "boot", ["boot/bootx64.c", "boot/bootlib.c"], implicit=find_files("boot/include", (".h",)))
    n.build("font/hankaku.o", "objcopy_bin", "font/hankaku.bin", variables={"bin_input": "./font/hankaku.bin"})
    n.build("out/kernel.krl", "kernel_link", kernel_objs + [Path("font/hankaku.o"), Path("liballoc-x86_64.a")], implicit=["linker.ld"])
    user_image_candidate_test = Path("out/tests/user_image_candidate_test")
    n.build(
        user_image_candidate_test,
        "host_cxx_test",
        ["tests/user_image_candidate_test.cpp", "kernel/user_image_candidate.cpp"],
        implicit=["include/user_image_candidate.h"],
        variables={"host_test_cxxflags": "-std=gnu++17 -I./include -DXJ380_CANDIDATE_HOST_TEST"},
    )
    compliance_bundle = Path("out/compliance/third-party/MANIFEST.json")
    compliance_manifest_path = Path("third_party/compliance-manifest.json")
    compliance_manifest = json.loads((ROOT / compliance_manifest_path).read_text(encoding="utf-8"))
    compliance_inputs = [
        Path("tools/package_third_party.py"),
        Path("tools/generate_lwip_notice.py"),
        compliance_manifest_path,
        Path("LICENSE"),
        Path("THIRD_PARTY_NOTICES.md"),
    ]
    for component in compliance_manifest["components"]:
        packaged_materials = list(component["license_files"])
        if component["bundle_source"]:
            packaged_materials.extend(component["source_files"])
        for material in packaged_materials:
            material_path = Path(material)
            if material_path not in compliance_inputs:
                compliance_inputs.append(material_path)
    n.build(
        compliance_bundle,
        "package_compliance",
        compliance_inputs,
    )

    n.comment("USERLAND AND KMODS - first-class ELF/module outputs")
    core_objs, constart_obj, xapi_liballoc = xapi(n)
    user_targets = user_apps(n, core_objs, constart_obj)
    kmod_targets = kmods(n)

    n.comment("PHONY TARGETS - compatibility names for common build workflows")
    all_deps = [Path("out/BOOTX64.efi"), Path("out/kernel.krl"), compliance_bundle, user_image_candidate_test] + user_targets + kmod_targets
    phony(n, "all", all_deps)
    phony(n, "build_xapi", core_objs + [constart_obj, xapi_liballoc])
    phony(n, "kmods", kmod_targets)
    phony(n, "test.user-image-candidate", [user_image_candidate_test])

    n.comment("IMAGE AND UTILITY TARGETS - delegate staging/run helpers to tools/ninja_build.py")
    # Image creation, QEMU launch, formatting, and graph reporting stay in the
    # helper script because they are imperative workflows rather than compile
    # edges.  Ninja still tracks their visible target names here.
    # License files are copied during staging.  Make staging targets depend on
    # an always-dirty phony input so an existing image is refreshed on every
    # request rather than being skipped by Ninja.
    n.build("always_stage_licenses", "phony", use_default_order_only=False)
    image_deps = all_deps + manifest_inputs + ["always_stage_licenses"]
    n.build(
        ["vdisk", "XJ380.img"],
        "python_cmd",
        image_deps,
        variables={"cmd": "vdisk", "desc": log_desc("VDISK", "XJ380.img"), "pool": "console"},
    )
    n.build(["vmdk", "XJ380.vmdk"], "python_cmd", ["vdisk"], variables={"cmd": "vmdk", "desc": log_desc("VMDK", "XJ380.vmdk")})
    n.build("run", "python_cmd", ["vdisk"], variables={"cmd": "run", "desc": log_desc("RUN", "QEMU"), "pool": "console"})
    n.build("justrun", "python_cmd", variables={"cmd": "run", "desc": log_desc("RUN", "QEMU"), "pool": "console"})
    n.build("format", "python_cmd", variables={"cmd": "format", "desc": log_desc("FMT", "sources")})
    n.build("check", "python_cmd", variables={"cmd": "check", "desc": log_desc("CHECK", "sources")})
    n.build("gen.clangd", "python_cmd", variables={"cmd": "gen-clangd", "desc": log_desc("GEN", "clangd")})
    n.build("size", "python_cmd", variables={"cmd": "size", "desc": log_desc("SIZE", "report")})
    n.build("check.tools", "python_cmd", variables={"cmd": "check-tools", "desc": log_desc("CHECK", "tools")})
    n.build("graph", "python_cmd", variables={"cmd": "graph", "desc": log_desc("GRAPH", "out/build-graph.dot")})
    n.build("clean.clangd", "shell_cmd", variables={"cmd": "rm -f .clangd boot/.clangd", "desc": log_desc("CLEAN", "clangd")})
    n.build("clean", "python_cmd", variables={"cmd": "clean", "desc": log_desc("CLEAN", "build outputs")})
    n.build("reconfigure", "regen", ["tools/gen_ninja.py", "tools/ninja_build.py"])
    n.line("default all")

    output = Path(args.out)
    changed = write_if_changed(output, "\n".join(n.lines) + "\n")
    if changed and args.fail_if_changed:
        raise SystemExit("build.ninja was refreshed; rerun ninja so it can load the updated graph")


if __name__ == "__main__":
    main()
