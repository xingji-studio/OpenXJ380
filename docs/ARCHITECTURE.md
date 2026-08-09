# OpenXJ380 架构与目录边界

本文说明项目的稳定分层、依赖方向和代码归属。它不描述内部 ABI 的全部细节；涉及启动顺序、
中断帧、页表或系统调用 ABI 的改动，仍应结合相应目录中的 `AGENTS.md` 审查。

## 启动与运行路径

```text
UEFI firmware
    -> boot/ (BOOTX64.efi)
    -> kernel/KernelMain
    -> kernel core + built-in driver/
    -> VFS/root filesystem
    -> user-space ELF applications
    -> optional kmod/ modules
```

引导程序只负责固件交互、内核装载和 `BOOT_CONFIG` 交接。内核初始化完成后，用户态程序通过
Linux 兼容系统调用和 XAPI 使用内核服务。

## 目录职责

| 目录 | 职责 | 依赖边界 |
|---|---|---|
| `boot/` | UEFI 引导、固件协议、内核 ELF 装载 | 不依赖内核堆、线程或 libc |
| `kernel/` | CPU、内存、任务、系统调用和核心启动 | 可依赖共享 `include/`，不能依赖用户态实现 |
| `driver/` | 必须随内核启动的内置驱动和文件系统 | 链接进内核；可选设备优先放入 `kmod/` |
| `graphics/`、`font/` | 内核侧图形、窗口与字体实现 | 属于内核镜像，不是用户态 UI 框架 |
| `include/` | 内核、驱动和模块共享的 ABI 头文件 | 头文件应保持小而明确，避免引入实现细节 |
| `kmod/` | 可选的可加载内核模块 | 入口为 `dlmain`，通过导出符号使用内核服务 |
| `user/xapi/` | 用户态启动代码和运行时 | 与内核系统调用声明保持同步 |
| `user/` | XAPI 用户态 API 与命令行示例 | 不直接访问内核对象或物理地址 |
| `tools/`、`tests/` | Ninja 构建图、镜像工具及宿主机测试 | 不进入目标系统运行时 |
| `third_party/` | 外部上游源码 | 除同步上游或安全补丁外不做风格化改写 |

## 依赖规则

1. 内核核心不得反向依赖用户态应用。
2. 可加载模块不得成为基本启动流程的强依赖。
3. 用户指针必须在系统调用边界经过校验或复制，VFS/驱动不得直接信任用户缓冲区。
4. 生成文件（`build.ninja`、`kernel/build_config.h`、`.clangd`）不手工维护。
5. 第三方目录保留上游风格；本地适配放在第一方包装层。

## 高风险契约

- `boot/` 与 HHDM/高半区页表布局必须同步。
- `kernel/intr/` 的保存寄存器布局是汇编、调度器和系统调用共同使用的 ABI。
- `include/` 与 `user/xapi/include/` 中的系统调用号和数据结构必须同步。
- VFS 节点、文件句柄和模块导出符号具有跨子系统生命周期，修改所有权前必须审计关闭和复制路径。
