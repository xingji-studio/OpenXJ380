# Install and Build

## Clone

```bash
git clone https://github.com/xingji-studio/StardustUI.git
cd StardustUI
```

## Platform backends

StardustUI currently builds for:

- `xj380`
- `linux`
- `windows`

The top-level `Makefile` selects a platform automatically when possible, and you can also override it with `PLATFORM=...`.

## Linux

The Linux backend currently uses SDL2 and SDL_ttf.

On Arch Linux:

```bash
sudo pacman -S sdl2 sdl2_ttf
make PLATFORM=linux
```

On Debian/Ubuntu :
``` bash
sudo apt update
sudo apt install libsdl2-dev libsdl2-ttf-dev

make PLATFORM=linux
``` 

On RHEL :
``` bash
#New version of RHEL/Rocky
sudo dnf install SDL2-devel SDL2_ttf-devel
#Old version of RHEL/CentOS
sudo yum install SDL2-devel SDL2_ttf-devel

make PLATFORM=linux
``` 

This produces:

```text
build/libStardustUI.a
```

## Windows

You can build the Windows backend with MinGW:

```bash
make PLATFORM=windows CXX=x86_64-w64-mingw32-g++
```

## XJ380

Build with:

```bash
make PLATFORM=xj380
```

This requires the XJ380 toolchain and related objects used by the repository's `Makefile`.

## Platform detection

If you do not define a platform macro manually, `settings.hpp` currently uses:

- `_WIN32` -> `STARDUSTUI_WINDOWS`
- `__linux__` -> `STARDUSTUI_LINUX`
- otherwise -> `XJ380`

## Build the examples

Current examples:

- `examples/helloworld`
- `examples/duckchat`

### `helloworld`

Linux:

```bash
cd examples/helloworld
make PLATFORM=linux
./build/linux/helloworld
```

Windows:

```bash
cd examples/helloworld
make PLATFORM=windows CXX=x86_64-w64-mingw32-g++
```

XJ380:

```bash
cd examples/helloworld
make PLATFORM=xj380
```

### `duckchat`

Linux:

```bash
cd examples/duckchat
make PLATFORM=linux
./build/linux/duckchat
```

Windows:

```bash
cd examples/duckchat
make PLATFORM=windows CXX=x86_64-w64-mingw32-g++
```

XJ380:

```bash
cd examples/duckchat
make PLATFORM=xj380
```

This example is a complete chat UI that includes:

- a first-launch setup page
- a `Save And Connect` button
- a `Reconnect` button on the chat page
- a message input box and `Send` button
- JSON config persistence
- a command-line theme name argument

For custom colors, generate a Material 3 theme first with:

- <https://archzero.top/MD3color/>

## Related pages

- [Quick Start](./quickstart.md)
- [Create a Window](./create_window.md)
- [Style System](./style.md)
- [Button component](./button.md)
- [TextBox component](./textbox.md)
- [ScrollBar component](./scrollbar.md)
- [Layout System](./layout.md)
- [Canvas Component](./canvas.md)
- [DuckChat tutorial](./duckchat_tutorial.md)
