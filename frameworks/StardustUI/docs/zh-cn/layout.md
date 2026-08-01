# 布局系统

StardustUI 当前提供了一个类似 flex 的容器控件：`FlexLayout`。

## 头文件

```cpp
#include "../../includes/components/flex.hpp"
```

## 用途

`FlexLayout` 可以把子控件按行或按列排列，并且支持把固定尺寸控件和可扩展控件组合在一起。

## API

```cpp
class FlexLayout : public base_component
```

构造函数：

```cpp
FlexLayout(int width, int height);
```

配置接口：

```cpp
void set_direction(Direction direction);
void set_align_items(Align align);
void set_justify_content(Justify justify);
void set_gap(int gap);
void set_padding(int padding);
```

添加子控件：

```cpp
void addComponent(base_component& component, int flex_grow = 0);
void addComponent(base_component* component, int flex_grow = 0);
```

## 排列方向

```cpp
FlexLayout::Row
FlexLayout::Column
```

- `Row`：子控件从左到右排列
- `Column`：子控件从上到下排列

## 对齐方式

```cpp
FlexLayout::AlignStart
FlexLayout::AlignCenter
FlexLayout::AlignEnd
FlexLayout::AlignStretch
```

它们控制的是交叉轴：

- 行布局时，影响垂直方向
- 列布局时，影响水平方向

## 主轴分布

```cpp
FlexLayout::JustifyStart
FlexLayout::JustifyCenter
FlexLayout::JustifyEnd
FlexLayout::JustifySpaceBetween
```

它们控制的是主轴：

- 行布局时，影响水平方向
- 列布局时，影响垂直方向

## `flex_grow`

添加子控件时可以传入 `flex_grow`。

例如：

```cpp
content.addComponent(sidebar, 0);
content.addComponent(main_column, 1);
```

含义：

- `sidebar` 保持自己的首选尺寸
- `main_column` 会占用剩余空间

## 首选尺寸

`FlexLayout` 会读取每个子控件的首选尺寸：

```cpp
virtual int get_preferred_width() const;
virtual int get_preferred_height() const;
```

例如：

- `Lable` 的首选尺寸来自文字宽度和解析后的字号
- `Canvas` 的首选尺寸来自它当前的边界尺寸

## 示例

当前布局相关示例位于：

```text
examples/duckchat/duckchat.cpp
```

`DuckChat` 这个示例构建出的结构大致是：

1. 根列布局
2. 顶部 header
3. 中间内容行
4. 左侧 sidebar + 右侧主列
5. 主内容 canvas + 底部行
6. 底部两个 canvas

## 示例片段

```cpp
FlexLayout root(860, 560);
root.set_pos(20, 20);
root.set_direction(FlexLayout::Column);
root.set_gap(16);
root.set_padding(16);

Canvas header(0, 96);
Canvas sidebar(180, 0);
Canvas main_canvas(0, 0);

FlexLayout content(0, 0);
content.set_direction(FlexLayout::Row);
content.set_gap(16);

content.addComponent(sidebar, 0);
content.addComponent(main_canvas, 1);

root.addComponent(header, 0);
root.addComponent(content, 1);
```

## 说明

- `FlexLayout` 本身也是控件，可以直接加到 `Window` 里。
- 支持嵌套布局。
- 在窗口主循环里，布局会继续调用子控件的 `update()`。
- 如果子控件请求重绘，布局会把这个请求继续向上传递。

## 相关文档

- [Canvas 控件](./canvas.md)
- [创建窗口](./create_window.md)
- [快速开始](./quickstart.md)
- [DuckChat 教程](./duckchat_tutorial.md)
