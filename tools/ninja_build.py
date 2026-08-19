#!/usr/bin/env python3
from __future__ import annotations

import argparse
import glob
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OVMF_FIRMWARE_CANDIDATES = (
    Path("/usr/share/edk2/x64/OVMF.4m.fd"),
    Path("/usr/share/edk2/x64/OVMF_CODE.4m.fd"),
    Path("/usr/share/OVMF/OVMF_CODE.fd"),
    Path("/usr/share/edk2-ovmf/x64/OVMF_CODE.fd"),
    Path("/usr/share/qemu/OVMF.fd"),
    Path("/usr/share/ovmf/OVMF.fd"),
)

def env(name: str, default: str) -> str:
    value = os.environ.get(name)
    return default if value is None else value


def env_bool(name: str, default: str) -> bool:
    return env(name, default) == "1"


def uid_root(prefix: str) -> Path:
    return Path(f"/tmp/{prefix}-{os.getuid()}")


def run(args: list[str], *, cwd: Path = ROOT, env_extra: dict[str, str] | None = None) -> None:
    proc_env = os.environ.copy()
    if env_extra:
        proc_env.update(env_extra)
    subprocess.run(args, cwd=str(cwd), env=proc_env, check=True)


def shell(command: str, *, cwd: Path = ROOT, env_extra: dict[str, str] | None = None) -> None:
    proc_env = os.environ.copy()
    if env_extra:
        proc_env.update(env_extra)
    subprocess.run(["bash", "-lc", command], cwd=str(cwd), env=proc_env, check=True)


def capture(args: list[str]) -> str:
    try:
        return subprocess.check_output(args, cwd=str(ROOT), text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""


def ovmf_firmware() -> Path:
    override = os.environ.get("OVMF_FIRMWARE")
    if override:
        firmware = Path(override).expanduser()
        if not firmware.is_absolute():
            firmware = ROOT / firmware
        if firmware.is_file():
            return firmware
        raise FileNotFoundError(f"OVMF_FIRMWARE does not point to a file: {firmware}")

    for firmware in OVMF_FIRMWARE_CANDIDATES:
        if firmware.is_file():
            return firmware

    raise FileNotFoundError(
        "missing OVMF firmware; install the distro OVMF/edk2-ovmf package "
        "or set OVMF_FIRMWARE=/path/to/OVMF.fd"
    )


def chmod_rw(path: Path) -> None:
    shell(f'chmod -R u+rwX "{path}" 2>/dev/null || true')


def reset_dir(path: Path, *, explain_fix: bool = False) -> None:
    chmod_rw(path)
    if path.exists() or path.is_symlink():
        try:
            shutil.rmtree(path)
        except Exception:
            if explain_fix:
                print(f"Cannot remove {path}; fix ownership with:", file=sys.stderr)
                print(f"  sudo chown -R {os.getuid()}:{os.getgid()} {path}", file=sys.stderr)
            raise


def cp(src: str | Path, dst: str | Path) -> None:
    run(["cp", "-a", str(src), str(dst)])


def cp_glob(pattern: str, dst: str | Path) -> None:
    matches = sorted(glob.glob(str(ROOT / pattern)))
    if not matches:
        raise FileNotFoundError(pattern)
    run(["cp", "-a", *matches, str(dst)])


def rm_rf(path: str | Path) -> None:
    run(["rm", "-rf", str(path)])


def mkdir(path: str | Path) -> None:
    Path(path).mkdir(parents=True, exist_ok=True)


def stage_base(root: Path) -> None:
    run(["sh", "tools/stage_image_base.sh", str(root)])


def copy_system_apps(root: Path) -> None:
    system = root / "apps/system"
    cp(ROOT / "out/shell.elf", system)
    cp(ROOT / "resources/system/config/init.conf", root / "system/config/init.conf")


def copy_common_assets(root: Path) -> None:
    return


def copy_modules(root: Path) -> None:
    for sys_file in sorted((ROOT / "out").glob("*.sys")):
        rm_rf(root / "mod" / f"{sys_file.stem}.km")
        cp(sys_file, root / "mod")


def copy_license_material(root: Path) -> None:
    destination = root / "system/licenses"
    mkdir(destination)
    cp(ROOT / "LICENSE", destination / "LICENSE")
    cp(ROOT / "LICENSES.md", destination / "LICENSES.md")
    cp(ROOT / "THIRD_PARTY_NOTICES.md", destination / "THIRD_PARTY_NOTICES.md")
    for license_file in sorted((ROOT / "licenses").glob("*.txt")):
        cp(license_file, destination / license_file.name)

def fix_links(root: Path) -> None:
    shell(f'find "{root}" -xtype l -delete')
    shell(
        "find "
        + str(root)
        + r""" -type l -exec sh -c 'for lnk do tmp=${lnk}.xj380-copy; cp -aL "$lnk" "$tmp" 2>/dev/null && rm -f "$lnk" && mv "$tmp" "$lnk" || { rm -rf "$tmp"; rm -f "$lnk"; }; done' sh {} +"""
    )


def stage_full_system(root: Path) -> None:
    stage_base(root)
    cp(ROOT / "out/BOOTX64.efi", root / "EFI/BOOT")
    cp(ROOT / "out/kernel.krl", root / "system")
    copy_system_apps(root)
    copy_common_assets(root)
    copy_modules(root)
    copy_license_material(root)
    cp(ROOT / "out/compliance/third-party", root / "system/licenses/third-party")
    fix_links(root)


def vdisk() -> None:
    image = ROOT / "XJ380.img"
    image.unlink(missing_ok=True)
    vdisk_root = Path(env("VDISK_ROOT", f"/tmp/xj380-vdisk-{os.getuid()}/XJ380"))
    reset_dir(vdisk_root, explain_fix=True)
    run(["dd", "if=/dev/zero", f"of={image}", "bs=1M", f"count={env('VDISK_SIZE_MIB', '3072')}"])
    sudo = env("VDISK_SUDO", "").strip()
    shell(f'{sudo + " " if sudo else ""}sgdisk XJ380.img -n 1:2048 -t 1:ef00')
    shell(
        f'{sudo + " " if sudo else ""}mkfs.vfat -F 32 --offset 2048 -S 512 '
        f'XJ380.img $(( {env("VDISK_FAT_MIB", "3072")} * 1024 ))'
    )
    stage_full_system(vdisk_root)
    shell(f'mcopy -o -s -i XJ380.img@@$((2048*512)) "{vdisk_root}"/* ::')
    reset_dir(vdisk_root, explain_fix=True)


def vmdk() -> None:
    (ROOT / "XJ380.vmdk").unlink(missing_ok=True)
    run(["qemu-img", "convert", "-O", "vmdk", "XJ380.img", "XJ380.vmdk"])


def qemu_cmd() -> str:
    debug_flag = "-s -S" if env_bool("DEBUG", "1") else ""
    sudo_cmd = ""
    if env_bool("SUDO", "1"):
        sudo_cmd = f"sudo -E env DISPLAY={os.environ.get('DISPLAY', '')} XAUTHORITY={os.environ.get('XAUTHORITY', '')} "
    kvm_flag = "--enable-kvm -cpu host" if env_bool("KVM", "1") else "-cpu max"
    if env_bool("USB_BOOT", "0"):
        storage = (
            "-drive if=none,id=usbdisk,file=XJ380.img,index=0,format=raw "
            "-device usb-storage,bus=xhci.0,port=3,drive=usbdisk,bootindex=0"
        )
    else:
        storage = (
            "-drive if=none,id=harddisk,file=XJ380.img,index=0,format=raw "
            "-device ahci,id=ahci -device ide-hd,bus=ahci.0,drive=harddisk,bootindex=0"
        )
    firmware = shlex.quote(str(ovmf_firmware()))
    return (
        f"{sudo_cmd}qemu-system-x86_64 --display {env('DISPLAY_BACKEND', 'gtk')} -M q35 -bios {firmware} "
        f"-m 8192 -smp {env('SMP', '4')} {kvm_flag} -device qemu-xhci,id=xhci {storage} "
        "-boot strict=on "
        f"{env('USB_XHCI_DEVICES', '-device usb-kbd,bus=xhci.0,port=1 -device usb-mouse,bus=xhci.0,port=2')} "
        f"-serial file:serial.log -audiodev {env('AUDIO_BACKEND', 'sdl')},id=snd0 "
        f"-device ich9-intel-hda -device {env('HDA_CODEC', 'hda-output')},audiodev=snd0 "
        "-netdev user,id=net0,hostfwd=tcp::2323-:2323 -device e1000,netdev=net0 "
        f"{debug_flag} -monitor stdio"
    )


def run_qemu() -> None:
    (ROOT / "serial.log").unlink(missing_ok=True)
    try:
        command = qemu_cmd()
    except FileNotFoundError as error:
        raise SystemExit(str(error)) from None
    shell(command)


def clean() -> None:
    for rel in ("out", "XJ380.img", "serial.log", "kernel/build_config.h"):
        rm_rf(ROOT / rel)
    print("Removed build files.")


def root_sources() -> tuple[list[Path], list[Path], list[Path]]:
    roots = [ROOT / p for p in ("kernel", "driver", "font", "lib")]
    c_files: list[Path] = []
    cpp_files: list[Path] = []
    h_files = list((ROOT / "include").rglob("*.h")) + list((ROOT / "include").rglob("*.hpp"))
    for base in roots:
        c_files.extend(base.rglob("*.c"))
        cpp_files.extend(base.rglob("*.cpp"))
    return sorted(c_files), sorted(cpp_files), sorted(h_files)


def format_sources() -> None:
    c_files, cpp_files, h_files = root_sources()
    for path in c_files + cpp_files + h_files:
        print(f"Formatting {path.relative_to(ROOT)}")
        run(["clang-format", "-i", str(path)])
    print("Formatted all the code ...")


def check_sources() -> None:
    c_files, cpp_files, _ = root_sources()
    c_flags = (
        "-O0 -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin "
        "-m64 -fno-stack-protector -fno-exceptions -std=c11 -fshort-wchar -nostdinc "
        "-mno-80387 -I./include"
    ).split()
    cpp_flags = (
        "-O0 -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin "
        "-m64 -fno-stack-protector -fno-exceptions -fno-rtti -std=gnu++17 -fshort-wchar "
        "-nostdinc -fno-use-cxa-atexit -fno-threadsafe-statics -mno-80387 "
        "-Wno-int-to-pointer-cast -Wno-macro-redefined -Wno-c11-extensions "
        "-Wno-c99-extensions -Wno-gnu-statement-expression-from-macro-expansion "
        "-I./include -Wno-writable-strings -Wno-c++11-narrowing"
    ).split()
    for path in c_files:
        print(f"Checking C {path.relative_to(ROOT)}")
        run(["clang-tidy", str(path), "--", *c_flags])
    for path in cpp_files:
        print(f"Checking CPP {path.relative_to(ROOT)}")
        run(["clang-tidy", str(path), "--", *cpp_flags])
    print("Checked all the code ...")


def gen_clangd() -> None:
    templates = sorted(ROOT.rglob(".clangd_template"))
    for template in templates:
        out = template.with_name(".clangd")
        out.unlink(missing_ok=True)
    workspace = str(ROOT)
    for template in templates:
        out = template.with_name(".clangd")
        text = template.read_text(encoding="utf-8").replace("${workspaceFolder}", workspace)
        out.write_text("# Generated by Ninja build\n" + text, encoding="utf-8")
        print(f"\033[1;32m[Done]\033[0m {out.relative_to(ROOT)} was generated.")
    print("\033[1;32m[Done]\033[0m .clangd configurations was generated.\n")


def human_size(size: int) -> str:
    value = float(size)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if value < 1024.0 or unit == "TB":
            return f"{value:.1f} {unit}" if unit != "B" else f"{int(value)} B"
        value /= 1024.0
    return f"{size} B"


def print_size_group(title: str, files: list[Path]) -> None:
    print(f"\n{title}")
    print("-" * 64)
    any_file = False
    for path in files:
        full = ROOT / path
        if not full.is_file():
            print(f"{path.as_posix():42} 未生成")
            continue
        any_file = True
        print(f"{path.as_posix():42} {human_size(full.stat().st_size):>12}")
    if not any_file:
        print("没有已生成的文件。")


def size_report() -> None:
    print("XJ380 构建产物大小")
    print("=" * 64)
    print_size_group("核心", [Path("out/kernel.krl"), Path("out/BOOTX64.efi")])
    print_size_group(
        "主要应用",
        [
            Path("out/shell.elf"),
        ],
    )
    print_size_group("内核模块", [Path("out/e1000.sys"), Path("out/netserver.sys"), Path("out/xhci.sys")])
    print_size_group(
        "镜像 / 安装介质",
        [
            Path("XJ380.img"),
            Path("XJ380.vmdk"),
        ],
    )


def find_tool(names: list[str]) -> str | None:
    extra_paths = [
        Path("/usr/sbin"),
        Path("/sbin"),
        Path("/usr/local/sbin"),
        Path.home() / ".cargo/bin",
        Path("/snap/bin"),
        Path("/home/leon/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin"),
    ]
    for name in names:
        found = shutil.which(name)
        if found:
            return found
        for base in extra_paths:
            candidate = base / name
            if candidate.exists() and os.access(candidate, os.X_OK):
                return str(candidate)
    return None


def env_tool(name: str, default: str) -> str:
    value = env(name, default).strip()
    if not value:
        return default
    return value.split()[0]


def check_required_tool(
    missing: list[tuple[str, str, str]],
    display: str,
    aliases: list[str],
    purpose: str,
    hint: str,
) -> str | None:
    found = find_tool(aliases)
    if found:
        print(f"[OK]   {display:28} {found}")
        return found

    print(f"[缺失] {display:28} {purpose}")
    missing.append((display, purpose, hint))
    return None


def check_tools() -> None:
    missing: list[tuple[str, str, str]] = []
    prefix = env("COMPILER_PREFIX", "")
    print("XJ380 Ninja 构建工具检查")
    print("=" * 64)

    check_required_tool(missing, "clang", [env_tool("CC", prefix + "clang"), "clang"],
                        "编译内核 C/ASM 和部分模块", "sudo apt install clang")
    check_required_tool(missing, "clang++", [env_tool("CPP", prefix + "clang++"), "clang++"],
                        "编译内核 C++ 和模块", "sudo apt install clang")
    check_required_tool(missing, "g++", [env_tool("CXX", "g++"), "g++"], "编译用户态 C++",
                        "sudo apt install g++")
    check_required_tool(missing, "ld", [env_tool("LD", prefix + "ld"), "ld"], "链接内核和用户态 ELF",
                        "sudo apt install binutils")
    check_required_tool(missing, "ld.lld", ["ld.lld", "lld"], "提供 LLVM lld 链接器，避免 CI/本地缺 lld 后分散失败",
                        "sudo apt install lld")
    check_required_tool(missing, "objcopy", [env_tool("OBJC", prefix + "objcopy"), "objcopy"],
                        "打包内置二进制资源", "sudo apt install binutils")
    check_required_tool(missing, "nasm", [env_tool("NASM", prefix + "nasm"), "nasm"], "编译 NASM 汇编",
                        "sudo apt install nasm")
    rustc = check_required_tool(missing, "rustc", [env_tool("RUSTC", "rustc"), "rustc"],
                                "编译 Rust no_std 用户态应用", "rustup toolchain install stable")
    check_required_tool(missing, "x86_64-w64-mingw32-gcc", ["x86_64-w64-mingw32-gcc"], "编译 UEFI bootloader",
                        "sudo apt install gcc-mingw-w64-x86-64")
    check_required_tool(missing, "python3", ["python3"], "生成 Ninja 和镜像工具",
                        "sudo apt install python3")
    check_required_tool(missing, "ninja", ["ninja", "ninja-build"], "执行 Ninja 构建",
                        "sudo apt install ninja-build")
    check_required_tool(missing, "mcopy", ["mcopy"], "写入 FAT 磁盘镜像", "sudo apt install mtools")
    check_required_tool(missing, "sgdisk", ["sgdisk"], "创建 GPT/EFI 分区", "sudo apt install gdisk")
    check_required_tool(missing, "mkfs.vfat", ["mkfs.vfat"], "格式化 FAT32 EFI 分区",
                        "sudo apt install dosfstools")
    check_required_tool(missing, "qemu-system-x86_64", ["qemu-system-x86_64"], "运行 XJ380",
                        "sudo apt install qemu-system-x86")
    check_required_tool(missing, "qemu-img", ["qemu-img"], "生成 VMDK 镜像", "sudo apt install qemu-utils")
    check_required_tool(missing, "xorriso", ["xorriso"], "生成 UEFI 安装 ISO", "sudo apt install xorriso")

    if rustc:
        rust_target = env("RUST_TARGET", "x86_64-unknown-none")
        libdir = env("RUST_TARGET_LIBDIR", "")
        if not libdir:
            libdir = capture([rustc, "--print", "target-libdir", "--target", rust_target])
        rust_libs_ok = False
        if libdir:
            rust_libs_ok = bool(list(Path(libdir).glob("libcore-*.rlib"))) and bool(
                list(Path(libdir).glob("libcompiler_builtins-*.rlib"))
            )
        if rust_libs_ok:
            print(f"[OK]   rust target {rust_target:14} {libdir}")
        else:
            print(f"[缺失] rust target {rust_target:14} 编译 Rust no_std 用户态应用需要该目标")
            missing.append((f"rust target {rust_target}", "缺少 libcore/libcompiler_builtins",
                            f"rustup target add {rust_target} 或设置 RUST_TARGET_LIBDIR"))

    if missing:
        print("\n缺失工具和建议安装命令：")
        for display, purpose, hint in missing:
            print(f"- {display}: {purpose}；建议：{hint}")
        raise SystemExit(1)

    print("\n所有关键工具都已找到。")


def graph() -> None:
    ninja = find_tool(["ninja", "ninja-build"])
    if not ninja:
        print("缺少 ninja，无法输出构建图。建议：sudo apt install ninja-build", file=sys.stderr)
        raise SystemExit(1)

    out_dir = ROOT / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    dot_path = out_dir / "build-graph.dot"
    targets = env("NINJA_GRAPH_TARGETS", "all vdisk").split()
    with dot_path.open("w", encoding="utf-8") as f:
        subprocess.run([ninja, "-t", "graph", *targets], cwd=str(ROOT), stdout=f, check=True)

    print(f"已输出 Ninja 构建图: {dot_path.relative_to(ROOT)}")
    dot = find_tool(["dot"])
    if dot:
        svg_path = out_dir / "build-graph.svg"
        subprocess.run([dot, "-Tsvg", str(dot_path), "-o", str(svg_path)], cwd=str(ROOT), check=True)
        print(f"已输出 SVG 构建图: {svg_path.relative_to(ROOT)}")
    else:
        print("未找到 graphviz dot，跳过 SVG。需要 SVG 可安装：sudo apt install graphviz")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("command")
    args = parser.parse_args()

    commands = {
        "vdisk": vdisk,
        "vmdk": vmdk,
        "run": run_qemu,
        "clean": clean,
        "format": format_sources,
        "check": check_sources,
        "gen-clangd": gen_clangd,
        "size": size_report,
        "check-tools": check_tools,
        "graph": graph,
    }
    try:
        commands[args.command]()
    except KeyError:
        parser.error(f"unknown command: {args.command}")


if __name__ == "__main__":
    main()
