# 快速开始

本页示例与当前 `examples/helloworld/helloworld.cpp` 保持一致。

## 1. 包含头文件

```cpp
#include "../../includes/window.hpp"
#include "../../includes/components/lable.hpp"
#include "../../includes/sytle.hpp"
```

## 2. 创建窗口

```cpp
Window window("Hello, World!", 400, 300);
```

## 3. 创建悬停样式

```cpp
Sytel base_style;
base_style.set_color(0x000000FF);
base_style.set_size(24);

Sytel hover_style;
hover_style.set_color(0xFF0000FF);

SytelRules rules;
rules.set_base_sytel(base_style);
rules.set_on_hover_sytel(hover_style);
```

## 4. 创建文字控件并绑定样式

```cpp
Lable hello_label("Hello, World!", 24, 0x000000FF);
hello_label.set_style_rules(rules);
hello_label.set_pos(100, 100);
```

## 5. 加入窗口并显示

```cpp
window.addComponent(hello_label);
window.show();
```

当鼠标移动到文字上方时，文字会变成红色。

## 完整示例

```cpp
#include "../../includes/window.hpp"
#include "../../includes/components/lable.hpp"
#include "../../includes/sytle.hpp"

int main(int argc, char *argv[], char *envp[])
{
    Window window("Hello, World!", 400, 300);

    Sytel base_style;
    base_style.set_color(0x000000FF);
    base_style.set_size(24);

    Sytel hover_style;
    hover_style.set_color(0xFF0000FF);

    SytelRules rules;
    rules.set_base_sytel(base_style);
    rules.set_on_hover_sytel(hover_style);

    Lable hello_label("Hello, World!", 24, 0x000000FF);
    hello_label.set_style_rules(rules);
    hello_label.set_pos(100, 100);

    window.addComponent(hello_label);
    window.show();
    return 0;
}
```

## 构建与运行

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

## 说明

- 当前文字控件类名是 `Lable`，不是 `Label`。
- 当前样式头文件名是 `sytle.hpp`，不是 `style.hpp`。
- `window.addComponent(...)` 现在同时支持引用和指针重载。
- 如果要做布局和自定义绘制，请看 [布局系统](./layout.md) 和 [Canvas 控件](./canvas.md)。
