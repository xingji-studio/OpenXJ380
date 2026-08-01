# DuckChat 教程

> 此程序由HGSpcae开发，非XingjiStudio产品，原Github链接 https://github.com/HGSpace-Studio/Duckchat 。  

这篇文档演示如何基于 StardustUI 一步步做出一个和 `examples/duckchat` 类似的聊天软件。

## 目标

最终程序包含：

- 一个窗口
- 一个首启配置页
- 一个聊天页
- 主机、端口、用户名输入框
- `Save And Connect`、`Reconnect`、`Send` 按钮
- 聊天记录区
- JSON 配置文件
- 命令行主题参数

参考实现：

```text
examples/duckchat/duckchat.cpp
```

## 第一步：准备主题

推荐先用在线 Material 3 主题生成器生成主题文件：

- <https://archzero.top/MD3color/>

生成后把主题文件放到：

- 用户目录：`$HOME/.config/stardustui/theme`
- 系统目录：`/etc/stardustui/theme`

然后程序里可以这样加载：

```cpp
const char* theme_to_load = "green_light";
if (argc > 1 && argv != nullptr && argv[1] != nullptr && argv[1][0] != '\0') {
    theme_to_load = argv[1];
}
stardustui::Theme::load_theme(theme_to_load);
```

## 第二步：创建窗口

```cpp
stardustui::string window_title("DuckChat");
Window window(window_title.c_str(), 1040, 680, true);
```

第四个参数表示窗口是否允许缩放。

## 第三步：搭建根容器

聊天软件通常不是单个控件，而是一组页面。

`DuckChat` 示例里用了一个 `ScreenHost` 作为页面切换容器：

```cpp
ScreenHost screen_host;
screen_host.set_bounds(20, 20, 1000, 640);
screen_host.set_anchors(base_component::AnchorLeft |
                        base_component::AnchorTop |
                        base_component::AnchorRight |
                        base_component::AnchorBottom);
window.addComponent(screen_host);
```

这样窗口缩放时，内容区域会跟着变化。

## 第四步：做首启配置页

配置页一般用列布局：

```cpp
FlexLayout setup_screen(1000, 640);
setup_screen.set_direction(FlexLayout::Column);
setup_screen.set_gap(16);
setup_screen.set_padding(28);
```

然后加入：

- 标题 `Lable`
- 描述 `Lable`
- 配置卡片 `FlexLayout`
- 三个 `TextBox`
- 一个提示 `Lable`
- 一个 `Save And Connect` 按钮

例如：

```cpp
Lable host_label("Server IP / Domain", 14, colors.on_surface_variant);
TextBox host_input(0, 58, true, make_textbox_rules(colors));

Lable port_label("Port", 14, colors.on_surface_variant);
TextBox port_input(0, 58, true, make_textbox_rules(colors));

Lable username_label("Username", 14, colors.on_surface_variant);
TextBox username_input(0, 58, true, make_textbox_rules(colors));
```

## 第五步：做聊天页

聊天页可以拆成左右两列：

- 左边侧栏
- 右边主内容

```cpp
FlexLayout chat_screen(1000, 640);
chat_screen.set_direction(FlexLayout::Row);
chat_screen.set_gap(12);
chat_screen.set_padding(0);
```

侧栏里可以放：

- `Connection`
- `Stored in JSON config`
- `Reconnect`

右侧主列里可以放：

- 顶部标题栏
- 聊天记录框
- 底部发送区

## 第六步：聊天记录和消息输入

`DuckChat` 示例里聊天记录区和输入区都复用了 `TextBox`。

聊天记录框：

```cpp
TextBox history_box(0, 0, false, make_textbox_rules(colors));
```

消息输入框：

```cpp
TextBox message_input(0, 92, true, make_textbox_rules(colors));
```

发送按钮：

```cpp
Button send_button("Send", 120, 92,
                   make_button_rules(colors,
                                     colors.primary,
                                     colors.on_primary,
                                     colors.secondary,
                                     colors.on_secondary));
send_button.callback(on_send_click);
```

## 第七步：接入网络

框架已经提供了 `network.hpp` 对应的 TCP / HTTP 能力。

聊天场景通常需要：

- 建立 TCP 连接
- 发送消息
- 非阻塞轮询接收

`DuckChat` 示例里用一个 `PollerComponent` 每帧轮询 socket：

```cpp
PollerComponent poller;
poller.set_update_proc(poll_chat_socket);
window.addComponent(poller);
```

这样窗口主循环会持续驱动网络收包，而不需要额外阻塞 UI 线程。

## 第八步：保存 JSON 配置

首启时把：

- host
- port
- username

写入 JSON 文件。

当前示例路径规则：

- Linux / Windows: `$HOME/.config/stardustui/chat.json`
- XJ380: `/etc/stardustui-chat.json`

你可以直接参考：

```text
examples/duckchat/duckchat.cpp
```

里面的：

- `load_config`
- `save_config`
- `apply_setup_inputs`

## 第九步：页面切换

启动时如果已经有配置：

- 直接进入聊天页
- 尝试连接服务器

如果没有配置：

- 显示配置页

思路是：

```cpp
if (g_config_loaded) {
    show_screen(&chat_screen);
    connect_chat();
} else {
    show_screen(&setup_screen);
}
```

## 第十步：编译运行

Linux：

```bash
cd examples/duckchat
make PLATFORM=linux
./build/linux/duckchat green_light
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

## 建议的开发顺序

建议按这个顺序开发：

1. 先把窗口和两个页面搭起来
2. 再把输入框和按钮放进去
3. 再接 JSON 配置读写
4. 再接 TCP 收发
5. 最后补主题、状态提示和细节文案

## 相关文件

- 示例源码：`examples/duckchat/duckchat.cpp`
- 示例构建：`examples/duckchat/Makefile`
- 主题系统：`src/theme.cpp`
- 网络接口：`src/network.cpp`

