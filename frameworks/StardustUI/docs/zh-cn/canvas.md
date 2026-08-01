# Canvas 控件

StardustUI 当前提供了一个 `Canvas` 控件，用于自定义像素级绘制。

## 头文件

```cpp
#include "../../includes/components/canvas.hpp"
```

## 用途

当内置文字控件不够用，或者你想自己绘制像素、色块时，就可以使用 `Canvas`。

早期 `layout` 示例里，每个布局区域的色块都是通过 `Canvas` 画出来的。

## API

```cpp
class Canvas : public base_component
```

构造函数：

```cpp
Canvas(int width, int height);
```

绘制接口：

```cpp
void clear();
void set_pixel(int x, int y, unsigned int color);
void fill_rect(int x, int y, int width, int height, unsigned int color);
```

刷新回调：

```cpp
using RefreshCallback = void (*)(Canvas&);
void set_refresh_callback(RefreshCallback callback);
```

## 刷新机制

刷新回调是在 `Canvas::update()` 中调用的。

由于 `Window::show()` 会在主循环中对每个控件执行一次 `update()`，所以 canvas 的回调也会在每轮主循环里执行一次。

当前流程是：

1. `Canvas::update()` 先清空上一帧命令
2. 调用刷新回调重新生成这一帧内容
3. canvas 主动请求重绘
4. 窗口重新绘制所有控件

因此 `Canvas` 适合做简单动画、调试可视化和自定义图形区域。

## 坐标

`Canvas` 的绘制坐标都是相对于它自身左上角的局部坐标。

例如：

```cpp
canvas.set_pixel(2, 2, 0x000000FF);
canvas.fill_rect(0, 0, 100, 40, 0xE85D5DFF);
```

真正绘制到窗口时，StardustUI 会再自动加上这个 canvas 在窗口中的位置偏移。

## 示例

```cpp
void draw_header(Canvas& canvas) {
    canvas.fill_rect(0, 0, canvas.get_width(), canvas.get_height(), 0xE85D5DFF);
    canvas.set_pixel(2, 2, 0x000000FF);
}

Canvas header(0, 96);
header.set_refresh_callback(draw_header);
```

## 说明

- `Canvas` 会自动裁剪超出范围的像素和矩形。
- `Canvas` 的首选尺寸来自它当前边界尺寸。
- `Canvas` 可以直接加入 `Window`，也可以作为 `FlexLayout` 的子控件使用。

## 相关文档

- [布局系统](./layout.md)
- [创建窗口](./create_window.md)
