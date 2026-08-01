# ScrollBar 控件

`ScrollBar` 是一个垂直滚动条控件，适合和文本区或自定义内容区域配合使用。

## 头文件

```cpp
#include "../../includes/components/scrollbar.hpp"
```

## 类定义

```cpp
class ScrollBar : public base_component
```

## 构造函数

```cpp
ScrollBar(int width, int height);
ScrollBar(int width, int height, const SytelRules& style);
```

## 常用接口

```cpp
void set_range(int content_size, int page_size);
void set_value(int value);
int get_value() const;
int get_content_size() const;
int get_page_size() const;
int get_max_value() const;
bool is_dragging() const;
void set_change_callback(void (*func)(ScrollBar&, int));
```

## 工作方式

滚动条根据：

- `content_size`
- `page_size`

计算最大滚动值和滑块尺寸。

例如：

```cpp
scrollbar.set_range(2000, 480);
scrollbar.set_value(120);
```

这表示：

- 总内容高度是 `2000`
- 当前可见区域高度是 `480`
- 当前滚动位置是 `120`

## 交互

`ScrollBar` 已实现：

```cpp
bool handle_pointer_move(int x, int y) override;
bool handle_left_button(bool pressed, int x, int y) override;
```

支持：

- 点击轨道
- 拖动滑块

## 在 TextBox 里的用途

当前 `TextBox` 内部已经组合了 `ScrollBar`，所以大多数文本滚动场景不需要你手动再放一个滚动条。

## 相关文档

- [TextBox 控件](./textbox.md)
- [DuckChat 教程](./duckchat_tutorial.md)
