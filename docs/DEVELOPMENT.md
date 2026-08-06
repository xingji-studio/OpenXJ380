# OpenXJ380 开发与维护规范

## 建议环境

使用 Linux 或 WSL，并安装 Python 3、Ninja、Clang/LLD、NASM 和 Rust。Rust 用户程序需要项目配置的
`RUST_TARGET` 目标；自定义工具链可以通过 `RUSTC` 和 `RUST_TARGET_LIBDIR` 指定。

## 标准验证流程

```bash
python3 -m unittest discover -s tests -v
python3 tools/gen_ninja.py --out build.ninja
ninja -f build.ninja check.tools
ninja -f build.ninja check
ninja -f build.ninja all
```

涉及镜像布局时再执行 `ninja -f build.ninja vdisk`；涉及启动、驱动、文件系统或用户可见行为时，
还应在 QEMU 中启动并检查串口日志。源码包未携带的镜像资源应通过获准的发布渠道准备，不要从历史
快照复制来源不明的二进制文件。

## 代码与注释

- 第一方 C/C++ 使用根目录 `.clang-format`，控制在 120 列以内。
- 注释解释约束、所有权、并发或硬件原因，不复述语句本身。
- 删除被替代的实现；不要长期保留整文件注释或无工单的 `#if 0` 代码。
- 调试输出必须有明确作用域，合入前删除一次性日志和个人路径。
- 新公共头文件使用 `#pragma once`，并只包含声明所需的最小依赖。

## 变更边界

- 修改系统调用时同步检查内核分发、XAPI 声明和调用者。
- 修改 VFS/文件句柄时检查 open、dup、close、进程复制和错误回滚路径。
- 修改启动或内存布局时同时审查 UEFI 装载、HHDM、链接脚本和页表初始化。
- 修改构建图时增加或更新 `tests/test_gen_ninja.py`，保证测试不依赖开发者个人工具链路径。

## 提交前清单

1. `git diff --check` 无空白错误。
2. 单元测试与 Ninja 图生成通过。
3. 第一方源码检查通过，未改写 vendored 目录。
4. 构建或运行受环境限制时，在审查说明中明确列出未执行项目及原因。
