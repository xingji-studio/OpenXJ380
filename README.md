# OpenXJ380

OpenXJ380 是一个面向 x86_64 平台的独立操作系统内核项目。项目包含 UEFI 引导程序、单体内核、XAPI 用户态 API、一个命令行示例程序、可加载内核模块，以及用于生成 FAT 磁盘镜像的构建和分发工具。

当前项目仍在积极开发中，适合对操作系统、内核、驱动和低层用户态运行时感兴趣的开发者参与和研究。

> GUI 状态：GUI 相关功能已于 2026-08-04 移除，原因是该实现尚不够成熟。当前项目不提供图形界面、窗口系统或 GUI 应用。

[贡献者名单](CONTRIBUTOR.md)

## 特性概览

- UEFI 引导与图形输出初始化。
- 面向 x86_64 的单体内核，内核入口为 `KernelMain`。
- 内建驱动、文件系统、进程/线程、内存管理和系统调用基础设施。
- XAPI 用户态运行时与 API，以及一个命令行示例程序。
- 可动态加载的内核模块，包括 E1000 网络、xHCI USB 和基于 lwIP 的网络服务。
- 面向可加载模块的产品扩展接口（OpenXJ380 Socket ABI）：键鼠中断钩子、帧缓冲配置、电源动作与系统调用钩子。
- 支持通过编译宏关闭内置图形、控制台与输入输出，构建无头（headless）内核。
- 基于 Ninja 的构建图生成、磁盘镜像制作和 QEMU 运行流程。

## 产品扩展接口（OpenXJ380 Socket ABI）

内核通过 `include/openxj380/` 下的头文件向可加载模块（`.sys`）开放一组产品级扩展接口（Socket ABI）。接口符号经 `EXPORT_SYMBOL` 导出，模块通过内核动态链接器解析调用。

- 键鼠扩展接口（`include/openxj380/socket.h`）：
  - `OpenXJ380Socket_RegisterMouseHook` / `OpenXJ380Socket_UnregisterMouseHook`
  - `OpenXJ380Socket_RegisterKeyboardHook` / `OpenXJ380Socket_UnregisterKeyboardHook`
  - 注册后，内核会在每个键鼠中断事件上回调钩子（`OpenXJ380Socket_MouseInterrupte` / `OpenXJ380Socket_KeyboardInterrupt`）。事件信息由 `OpenXJ380MouseInterruptInfo` / `OpenXJ380KeyboardInterruptInfo` 结构体承载，包含来源（PS/2、USB）、路由、按钮/滚轮增量、坐标、键值与 Shift/Ctrl/Alt/Win/Caps 等修饰键状态。控制器 I/O 与中断应答仍由内核持有，模块只能读取事件，不能从可加载模块操作控制器寄存器。
  - `OpenXJ380Socket_FramebufferConfig` 返回帧缓冲配置指针。
  - `OpenXJ380Socket_PowerAction` 触发重启或关机（`XPOWER_REBOOT` / `XPOWER_SHUTDOWN`）。
- 系统调用钩子（`include/openxj380/syscall.h`）：`OpenXJ380Socket_RegisterSyscallHook` / `OpenXJ380Socket_UnregisterSyscallHook` / `OpenXJ380Socket_DispatchSyscall`，可在系统调用分发路径上拦截或扩展。
- 内核动态链接器同时支持解析 C++ 修饰符号名与 `EXPORT_SYMBOL_OBJECT` 导出的对象。

## 无头构建（关闭内置输入输出）

内核支持编译宏 `OPENXJ380CONFIG_CLEAR_RUN`，用于关闭内置图形、控制台与输入输出。将该宏加入内核及内置驱动的编译选项（例如向 `CPP_FLAGS`/`C_FLAGS` 追加 `-DOPENXJ380CONFIG_CLEAR_RUN`）后，`include/openxj380/config.h` 会将以下宏置 1：

- `OPENXJ380_GUI_DISABLED`：禁用系统调用钩子等 GUI 相关能力。
- `OPENXJ380_CONSOLE_DISABLED`：`console_init` / `console_write` 变为空操作。
- `OPENXJ380_INPUT_OUTPUT_DISABLED`：关闭内置输入输出，包括 PS/2 键盘鼠标初始化、串口输出、XHCI/USB 初始化、HDA 音频与 XAPI 输入输出（`Input` / `Output` / `Getch` 等），并跳过模块自动装载。

关闭内置输入输出后，键鼠事件仍会通过上述 Socket ABI 钩子转发给可加载模块，由产品层自行处理输入。

## 目录结构

```text
boot/                  UEFI 引导程序与 EFI 交接逻辑
kernel/                内核核心子系统和启动入口
driver/                链接到内核镜像的内建驱动
graphics/、font/       不参与当前构建的历史图形与字体代码
include/               内核和模块共享的 ABI 头文件
lib/                   基础支持代码
kmod/                  可加载内核模块
user/                  XAPI 运行时/API 与一个命令行示例程序
resources/             写入系统资源目录的文件
Bf/                    可选的镜像/软件包静态资源（部分源码包不附带）
third_party/           引入的第三方代码
```

## 开发环境

推荐使用 Linux 或 WSL。以下命令以 Debian/Ubuntu 为例，安装完整的构建、镜像和 QEMU 运行依赖：

```bash
sudo apt update
sudo apt install -y \
    clang lld nasm ninja-build \
    mtools gdisk dosfstools \
    qemu-system-x86 qemu-utils ovmf
```

构建依赖 `Python 3`、Clang/LLD、NASM 和 Ninja。生成镜像还需要 `mtools`、`gdisk` 与 `dosfstools`；运行镜像需要 QEMU。可通过生成后的 Ninja 目标检查本机工具链：

```bash
python3 tools/gen_ninja.py --out build.ninja
ninja -f build.ninja check.tools
```


Rust no-std 目标库可通过 `RUST_TARGET_LIBDIR` 显式指定，避免构建图生成依赖开发者个人 rustup 路径。详细说明见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 构建

构建图由 `tools/gen_ninja.py` 生成。首次构建或构建规则更新后，执行：

```bash
python3 tools/gen_ninja.py --out build.ninja
ninja -f build.ninja all
```

`all` 会构建 UEFI 引导程序、内核、XAPI、命令行示例程序和默认内核模块。产物位于 `out/`，包括 `BOOTX64.efi`、`kernel.krl`、`shell.elf` 和模块文件。

常用目标：

```bash
ninja -f build.ninja kmods       # 仅构建可加载内核模块
ninja -f build.ninja check       # 运行源码检查
ninja -f build.ninja format      # 格式化第一方源码
ninja -f build.ninja gen.clangd  # 生成 clangd 配置
ninja -f build.ninja clean       # 删除构建产物
```

## 创建镜像与运行

构建完成后，使用以下命令制作 UEFI 可启动磁盘镜像：

```bash
ninja -f build.ninja vdisk
```

该命令生成 `XJ380.img`，并将内核、模块、命令行示例程序和资源按系统目录布局写入镜像。镜像制作通常需要写入分区和文件系统的权限；默认配置下，运行 QEMU 时也可能使用 `sudo`。

使用 QEMU 启动并在需要时自动创建镜像：

```bash
ninja -f build.ninja run
```

已有 `XJ380.img` 时可直接启动：

```bash
ninja -f build.ninja justrun
```

常用运行参数可通过环境变量调整：

```bash
DEBUG=0 SMP=2 SUDO=0 KVM=0 DISPLAY_BACKEND=gtk ninja -f build.ninja run
```

- `DEBUG`：默认 `1`，启用 QEMU 调试端口并在启动时暂停。
- `SMP`：虚拟 CPU 数量，默认 `4`。
- `SUDO`：是否通过 `sudo` 启动需要权限的流程，默认 `1`。
- `KVM`：是否启用 KVM，默认 `1`。
- `DISPLAY_BACKEND`：QEMU 显示后端，默认 `gtk`。
- `OVMF_FIRMWARE`：UEFI 固件路径。默认会查找常见发行版路径，例如
  `/usr/share/edk2/x64/OVMF.4m.fd` 和 `/usr/share/OVMF/OVMF_CODE.fd`。

## 开发说明

- 内核核心代码位于 `kernel/`；只有必须随内核启动的驱动才应放入 `driver/`。
- 可选或可动态装载的功能应放入 `kmod/`，模块入口为 `dlmain`。
- `user/` 仅保留 `user/xapi/` 提供的运行时/API 和 `cli_shell.cpp` 命令行示例；不再包含其他用户态应用或 GUI 实现。
- `third_party/` 和 `kmod/netserver/lwip/` 中的代码来自上游，除非进行有计划的上游同步，否则不要直接修改。
- `kernel/build_config.h` 和 `.clangd` 是生成文件，不应手动维护。
- 架构边界见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，可复现构建和镜像资源说明见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

版本信息目前需要同时更新 `tools/stage_image_base.sh` 与 `kernel/build_settings.h`。

## 许可与第三方组件

本项目采用 [Apache License 2.0](LICENSE)。仓库包含多个第三方组件，其各自的许可和分发要求见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 及对应源码目录。

第三方组件所要求公开的许可证已存放在 /usr/share/doc/xj380 下。
