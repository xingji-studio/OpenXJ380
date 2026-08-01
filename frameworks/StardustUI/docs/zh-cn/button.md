# Button 控件

`Button` 是 StardustUI 里最直接的可点击控件。

## 头文件

```cpp
#include "../../includes/components/button.hpp"
```

## 类定义

```cpp
class Button : public base_component
```

## 构造函数

```cpp
Button(const stardustui::string& text, int width, int height);
Button(const stardustui::string& text, int width, int height, const SytelRules& style);
```

参数说明：

- `text`：按钮文字
- `width`：按钮宽度
- `height`：按钮高度
- `style`：可选样式规则

## 常用接口

```cpp
void set_text(const stardustui::string& text);
const stardustui::string& get_text() const;
```

`Button` 的点击处理继承自 `base_component`，通常这样绑定：

```cpp
button.callback(on_button_click);
```

## 示例

```cpp
Button send_button("Send", 120, 48,
                   make_button_rules(colors,
                                     colors.primary,
                                     colors.on_primary,
                                     colors.secondary,
                                     colors.on_secondary));
send_button.callback(on_send_click);
```

## 在 DuckChat 里的用途

`examples/duckchat/duckchat.cpp` 里有三个按钮：

- `Save And Connect`
- `Reconnect`
- `Send`

这些按钮分别对应：

- 保存配置并建立连接
- 重新连接聊天服务器
- 发送当前输入框内容

## 相关文档

- [TextBox 控件](./textbox.md)
- [样式系统](./style.md)
- [DuckChat 教程](./duckchat_tutorial.md)
