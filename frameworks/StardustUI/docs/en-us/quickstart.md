# Quick Start

This quick start follows the current `examples/helloworld/helloworld.cpp`.

## 1. Include headers

```cpp
#include "../../includes/window.hpp"
#include "../../includes/components/lable.hpp"
#include "../../includes/sytle.hpp"
```

## 2. Create a window

```cpp
Window window("Hello, World!", 400, 300);
```

## 3. Create hover style

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

## 4. Create a label and attach style

```cpp
Lable hello_label("Hello, World!", 24, 0x000000FF);
hello_label.set_style_rules(rules);
hello_label.set_pos(100, 100);
```

## 5. Add the component and show the window

```cpp
window.addComponent(hello_label);
window.show();
```

When the mouse moves over the text, the label changes to red.

## Full example

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

## Build and run

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

## Notes

- The current component class name is `Lable`, not `Label`.
- The current style header is `sytle.hpp`, not `style.hpp`.
- `window.addComponent(...)` accepts both a reference and a pointer overload.
- For layout and custom drawing, see [Layout System](./layout.md) and [Canvas Component](./canvas.md).
