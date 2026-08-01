# TextBox 控件

`TextBox` 用于显示和输入文本。

它既可以作为只读文本区域使用，也可以作为可编辑输入框使用。

## 头文件

```cpp
#include "../../includes/components/textbox.hpp"
```

## 类定义

```cpp
class TextBox : public base_component
```

## 构造函数

```cpp
TextBox(int width, int height, bool input = true);
TextBox(int width, int height, bool input, const SytelRules& style);
```

参数说明：

- `width`：控件宽度
- `height`：控件高度
- `input`：是否允许输入
- `style`：可选样式规则

## 常用接口

```cpp
void set_text(const stardustui::string& text);
const stardustui::string& get_text() const;
void set_input_enabled(bool enabled);
bool is_input_enabled() const;
bool set_focus(bool focused) override;
```

## 交互接口

`TextBox` 已实现这些输入相关接口：

```cpp
bool handle_pointer_move(int x, int y) override;
bool handle_left_button(bool pressed, int x, int y) override;
bool handle_char_input(char ch, bool special) override;
```

这意味着它可以：

- 接收鼠标悬停和点击
- 获取焦点
- 处理字符输入
- 处理滚动条拖动

## 内置滚动条

`TextBox` 内部组合了一个 `ScrollBar`，用于长文本滚动。

因此它适合：

- 多行消息记录
- 长文本展示
- 可滚动输入内容

## 示例

只读聊天记录框：

```cpp
TextBox history_box(0, 0, false, make_textbox_rules(colors));
```

可输入消息框：

```cpp
TextBox message_input(0, 92, true, make_textbox_rules(colors));
```

设置文本：

```cpp
message_input.set_text("hello");
```

## 在 DuckChat 里的用途

`examples/duckchat/duckchat.cpp` 里 `TextBox` 用在：

- 服务器地址输入
- 端口输入
- 用户名输入
- 聊天记录显示
- 消息输入

## 相关文档

- [ScrollBar 控件](./scrollbar.md)
- [Button 控件](./button.md)
- [DuckChat 教程](./duckchat_tutorial.md)
