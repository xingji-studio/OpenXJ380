# OpenXJ380 编译与启动指南

本文档总结在 Windows 11 + WSL2 (Ubuntu) 环境下从零开始编译 OpenXJ380、
生成磁盘镜像并在 QEMU 中启动验证的完整流程。构建主机为 32 核 x86_64。

## 1. 环境要求

| 组件 | 版本 | 用途 |
|------|------|------|
| WSL2 + Ubuntu | Ubuntu 26.04 LTS | 构建与运行环境 |
| Python 3 | 3.14 (Win) / 3.13 (WSL) | 生成构建图 |
| Clang / LLD | 21.1.8 | 内核、驱动、模块编译与链接 |
| Ninja | 1.13.2 | 构建驱动 |
| NASM | 3.01 | `.asm` 汇编 |
| Rust (rustc) | 随 WSL 安装 | Rust 用户程序 (`RUST_TARGET=x86_64-unknown-none`) |
| mtools / gdisk / dosfstools | 随 WSL 安装 | GPT + FAT32 镜像制作 (`mkfs.vfat`, `mcopy`, `sgdisk`) |
| QEMU | 10.2.1 | 虚拟机启动验证 |
| OVMF | 随 `ovmf` 包安装 | UEFI 固件 (仓库根目录已自带 `OVMF.fd`) |

> 说明：镜像制作依赖 Linux 工具（`sgdisk`、`mkfs.vfat`、`mcopy`），因此整个构建流程
> 在 WSL2 Ubuntu 中执行。仓库根目录的 `liballoc-x86_64.a` 为预置第三方库，无需下载。

## 2. 工具链安装

```bash
# 在 WSL2 Ubuntu 中执行（如无免密 sudo 可用 wsl -u root）
sudo apt-get update
sudo apt-get install -y build-essential clang lld llvm ninja-build nasm \
    mtools gdisk dosfstools python3 git qemu-system-x86 ovmf
```

## 3. 编译步骤

```bash
# 进入仓库（Windows 路径 C:\Users\mnsjt\OpenXJ380 对应 /mnt/c/Users/mnsjt/OpenXJ380）
cd /mnt/c/Users/mnsjt/OpenXJ380

# 1. 生成构建图（构建图不允许手改，修改需改 tools/gen_ninja.py）
python3 tools/gen_ninja.py --out build.ninja

# 2. 全量编译（boot/kernel/xapi/user/kmod/busybox 等）
ninja -f build.ninja all -j 24
```

`ninja all` 产出物摘要：

| 产物 | 说明 |
|------|------|
| `out/kernel.krl` | 内核镜像（kernel + 内建驱动 + 字体） |
| `out/BOOTX64.efi` | UEFI 启动器 |
| `out/*.sys` | 可加载内核模块（xhci、e1000、netserver） |
| `out/apps/*` | 用户 ELF 程序 / busybox |

可选检查：`ninja -f build.ninja check`（单元测试与构建图自检）。

## 4. 生成磁盘镜像

```bash
ninja -f build.ninja vdisk
```

生成 `XJ380.img`（约 3.2 GiB）：GPT 分区表 + FAT32 根分区，镜像内布局为
`\system\kernel.krl`、`\EFI\BOOT\BOOTX64.EFI`、`\mod\*.sys`、`\apps` 等。

## 5. QEMU 启动验证

### 5.1 图形界面启动（默认）

```bash
# 注意：项目默认 DEBUG=1，会附加 -s -S（QEMU 启动即暂停等待调试器）
DEBUG=0 ninja -f build.ninja run
```

项目默认参数：`SMP=4`、`KVM=1`、`DISPLAY_BACKEND=gtk`。需要 X/WSLg 显示环境。

### 5.2 无人值守启动（headless，推荐验证用）

WSL2 下若 KVM 不可用（报 `Could not access KVM kernel module`），改用 TCG：

```bash
timeout 240 qemu-system-x86_64 -accel tcg,thread=multi -display none -M q35 \
    -bios OVMF.fd -cpu max -m 4096 -smp 4 \
    -device qemu-xhci,id=xhci \
    -drive if=none,id=harddisk,file=XJ380.img,index=0,format=raw \
    -device ahci,id=ahci -device ide-hd,bus=ahci.0,drive=harddisk,bootindex=0 \
    -boot strict=on \
    -device usb-kbd,bus=xhci.0,port=1 -device usb-mouse,bus=xhci.0,port=2 \
    -serial file:serial.log \
    -audiodev none,id=snd0 -device ich9-intel-hda -device hda-duplex,audiodev=snd0 \
    -netdev user,id=net0,hostfwd=tcp::2323-:2323 -device e1000,netdev=net0
```

> `-display none` 不需要 X 环境；`-serial file:serial.log` 将系统串口日志写入文件，
> 启动完成后检查 `serial.log` 即可判定启动是否成功。

## 6. 验证结果（2026-08-05 实测）

`serial.log` 关键里程碑：

```
EFI Initialize Success.                    ← UEFI 启动器加载
New Page Table Created Success.            ← 内核页表
Root Dir Open Success. / XSK2.1 Kernel Read Success.
Video Initialize Success.                  ← GOP 显存映射
FADT/MADT/HPET/MCFG Found Success.         ← ACPI 表解析
IDT/GDT Initialize Success.
Initializing SMP...  →  CPU 1..3 Initialize Success.  ← 4 核 SMP 就绪
Registers (stdio/tty/null/urandom) device success
XJ380 Message Initialize Success.          ← 内核消息系统
kmod: Module e1000 / netserver 加载成功     ← 可加载内核模块
e1000: Found Intel E1000 network controller（MAC 52:54:00:12:34:56）
Process Reaper is ready
Creating User Process. Name: shell（PID 1）
netserver: lwIP ready on e0                ← 网络栈
dhcp_recv: msg_type=5 yiaddr=10.0.2.15     ← DHCP 获取到 IP
XJ380 CLI
Username:                                  ← 到达登录提示符，启动成功
```

结论：在 TCG（`-cpu max`，无 KVM）下约 3 分 20 秒完成启动到 CLI 登录提示符；
全程无崩溃、无挂起，内核、驱动、文件系统、网络栈、用户进程均正常初始化。

## 7. 常见问题

| 现象 | 原因与解决 |
|------|-----------|
| `sudo: timed out` | WSL 用户无免密 sudo，改用 `wsl -u root` 安装工具链 |
| `Could not access KVM kernel module` | WSL2 未启用嵌套虚拟化，`-enable-kvm` 改 `-accel tcg,thread=multi -cpu max` |
| `ninja vdisk` 报找不到 sgdisk/mcopy | 缺 `gdisk`/`mtools`/`dosfstools` 包 |
| `ninja run` 启动即暂停 | 默认 `DEBUG=1` 附带 `-s -S`，调试器未连接前不会运行；置 `DEBUG=0` |
| 镜像内提示 block count mismatch | `mkfs.fat` 对磁盘大小取整的正常警告，不影响启动 |

## 8. 相关文件

- 构建图生成器：`tools/gen_ninja.py`；QEMU 命令拼装：`tools/ninja_build.py`
- 镜像根目录脚本：`tools/stage_image_base.sh`
- 启动验证产物：`serial.log`（根目录，每次运行覆盖）
