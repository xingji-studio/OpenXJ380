# 样式系统

StardustUI 当前的样式系统由两个核心类组成：

- `Sytel`
- `SytelRules`

这里的命名以实际代码为准，保持和仓库中的拼写一致。

## 头文件

```cpp
#include "../../includes/sytle.hpp"
```

## `Sytel`

`Sytel` 表示一组单独的样式属性，每个属性都是可选的。

### 支持的属性

```cpp
void set_color(unsigned int color);
void set_size(unsigned int size);
void set_background_color(unsigned int color);
void set_border_color(unsigned int color);
void set_border_width(unsigned int width);
void set_radius(unsigned int radius);
void set_padding(unsigned int padding);
```

每个属性也都带有：

- `unset_*()`
- `has_*() const`
- `get_*(fallback) const`

另外还有几个通用方法：

```cpp
void clear();
void merge_from(const Sytel& sytel);
bool empty() const;
```

## `SytelRules`

`SytelRules` 用来保存不同状态下的样式：

```cpp
void set_base_sytel(const Sytel& sytel);
void set_on_mouse_sytel(const Sytel& sytel);
void set_on_click_sytel(const Sytel& sytel);
void set_on_hover_sytel(const Sytel& sytel);
```

最终解析顺序为：

1. base
2. on mouse
3. on click
4. on hover

实际合并逻辑在 `SytelRules::resolve(bool on_mouse, bool on_click, bool on_hover)` 中。

## 把样式应用到控件

所有控件都继承自 `base_component`，它提供了：

```cpp
void set_style_rules(const SytelRules& rules);
const SytelRules& get_style_rules() const;
void clear_style_rules();
Sytel resolve_style() const;
```

## Hover 示例

下面就是当前 `helloworld` 的写法：

```cpp
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
```

效果：

- 默认文字颜色为黑色
- 鼠标悬停时文字变为红色

## 关于字号

对于 `Lable` 这类文字控件，最终字号按下面的逻辑解析：

```cpp
resolved_size = style.get_size(constructor_size);
```

也就是说：

- 构造函数里的 `size` 是默认值
- `Sytel::set_size(...)` 会在对应状态下覆盖这个默认值

至于最终显示出来有多大，取决于当前平台后端的具体实现。

## 当前已接入的控件

目前样式系统已经接入：

- `base_component`
- `Lable`
- `Canvas`
- `FlexLayout`

`Lable::draw(...)` 当前会读取：

- 文字颜色
- 文字大小

`Lable::contains(...)` 也会使用解析后的文字大小来计算 hover 命中区域。

`Canvas` 和 `FlexLayout` 同样继承了这一套基础样式与重绘机制，但它们目前还不会自动把 `Sytel` 里的边框、背景色、圆角、内边距直接绘制出来。这部分更适合作为后续扩展继续完善。

## 相关文档

- [创建窗口](./create_window.md)
- [快速开始](./quickstart.md)
- [布局系统](./layout.md)
- [Canvas 控件](./canvas.md)
