# 安装与构建

## 克隆仓库

```bash
git clone https://github.com/xingji-studio/StardustUI.git
cd StardustUI
```

## 当前平台后端

StardustUI 目前支持：

- `xj380`
- `linux`
- `windows`

顶层 `Makefile` 会尽量自动选择平台，也可以手动指定 `PLATFORM=...`。

## Linux

当前 Linux 后端使用 SDL2 和 SDL_ttf。

在 Arch Linux 上：

```bash
sudo pacman -S sdl2 sdl2_ttf
make PLATFORM=linux
```

在 Debian/Ubuntu 上：
``` bash
sudo apt update
sudo apt install libsdl2-dev libsdl2-ttf-dev

make PLATFORM=linux
``` 

在 RHEL 上：
``` bash
#新版本RHEL/Rocky
sudo dnf install SDL2-devel SDL2_ttf-devel
#老版本RHEL/CentOS
sudo yum install SDL2-devel SDL2_ttf-devel

make PLATFORM=linux
``` 

产物位置：

```text
build/libStardustUI.a
```

## Windows

可以使用 MinGW 交叉编译：

```bash
make PLATFORM=windows CXX=x86_64-w64-mingw32-g++
```

## XJ380

构建方式：

```bash
make PLATFORM=xj380
```

这个目标依赖仓库里现有的 XJ380 工具链和相关对象文件。

## 平台自动识别

如果不手动指定平台宏，`settings.hpp` 当前规则是：

- `_WIN32` -> `STARDUSTUI_WINDOWS`
- `__linux__` -> `STARDUSTUI_LINUX`
- 其他情况 -> `XJ380`

## 构建示例

当前示例包括：

- `examples/helloworld`
- `examples/duckchat`

### `helloworld`

Linux：

```bash
cd examples/helloworld
make PLATFORM=linux
./build/linux/helloworld
```

Windows：

```bash
cd examples/helloworld
make PLATFORM=windows CXX=x86_64-w64-mingw32-g++
```

XJ380：

```bash
cd examples/helloworld
make PLATFORM=xj380
```

### `duckchat`

Linux：

```bash
cd examples/duckchat
make PLATFORM=linux
./build/linux/duckchat
```

Windows：

```bash
cd examples/duckchat
make PLATFORM=windows CXX=x86_64-w64-mingw32-g++
```

XJ380：

```bash
cd examples/duckchat
make PLATFORM=xj380
```

这个示例是一个完整聊天界面，包含：

- 首次启动配置页
- `Save And Connect` 按钮
- 聊天页 `Reconnect` 按钮
- 消息输入框和 `Send` 按钮
- JSON 配置保存
- 命令行主题名参数

如果需要自定义主题，推荐先使用在线 Material 3 主题生成器生成配色：

- <https://archzero.top/MD3color/>

## 相关文档

- [快速开始](./quickstart.md)
- [创建窗口](./create_window.md)
- [样式系统](./style.md)
- [Button 控件](./button.md)
- [TextBox 控件](./textbox.md)
- [ScrollBar 控件](./scrollbar.md)
- [布局系统](./layout.md)
- [Canvas 控件](./canvas.md)
- [DuckChat 教程](./duckchat_tutorial.md)
