# DuckChat Tutorial

This page shows how to build a chat application similar to `examples/duckchat` with StardustUI.

## Goal

The final program includes:

- one window
- a first-launch setup screen
- a chat screen
- host, port, and username text boxes
- `Save And Connect`, `Reconnect`, and `Send` buttons
- a message history area
- JSON config persistence
- a command-line theme argument

Reference implementation:

```text
examples/duckchat/duckchat.cpp
```

## Step 1: Prepare a theme

Generate a Material 3 theme first with:

- <https://archzero.top/MD3color/>

Then place the generated theme files in:

- user directory: `$HOME/.config/stardustui/theme`
- system directory: `/etc/stardustui/theme`

Load a theme like this:

```cpp
const char* theme_to_load = "green_light";
if (argc > 1 && argv != nullptr && argv[1] != nullptr && argv[1][0] != '\0') {
    theme_to_load = argv[1];
}
stardustui::Theme::load_theme(theme_to_load);
```

## Step 2: Create a window

```cpp
stardustui::string window_title("DuckChat");
Window window(window_title.c_str(), 1040, 680, true);
```

The fourth argument controls whether the window is resizable.

## Step 3: Build the root host

A chat app usually needs multiple screens instead of one component.

The `DuckChat` example uses `ScreenHost` as the page switcher:

```cpp
ScreenHost screen_host;
screen_host.set_bounds(20, 20, 1000, 640);
screen_host.set_anchors(base_component::AnchorLeft |
                        base_component::AnchorTop |
                        base_component::AnchorRight |
                        base_component::AnchorBottom);
window.addComponent(screen_host);
```

This lets the content resize with the window.

## Step 4: Build the setup screen

The setup screen is a column layout:

```cpp
FlexLayout setup_screen(1000, 640);
setup_screen.set_direction(FlexLayout::Column);
setup_screen.set_gap(16);
setup_screen.set_padding(28);
```

Then add:

- a title `Lable`
- a description `Lable`
- a card `FlexLayout`
- three `TextBox` controls
- a hint `Lable`
- a `Save And Connect` button

Example:

```cpp
Lable host_label("Server IP / Domain", 14, colors.on_surface_variant);
TextBox host_input(0, 58, true, make_textbox_rules(colors));

Lable port_label("Port", 14, colors.on_surface_variant);
TextBox port_input(0, 58, true, make_textbox_rules(colors));

Lable username_label("Username", 14, colors.on_surface_variant);
TextBox username_input(0, 58, true, make_textbox_rules(colors));
```

## Step 5: Build the chat screen

The chat screen can be split into two columns:

- left sidebar
- right main area

```cpp
FlexLayout chat_screen(1000, 640);
chat_screen.set_direction(FlexLayout::Row);
chat_screen.set_gap(12);
chat_screen.set_padding(0);
```

The sidebar can contain:

- `Connection`
- `Stored in JSON config`
- `Reconnect`

The main area can contain:

- a top header
- a history box
- a bottom composer

## Step 6: Message history and input

The `DuckChat` example reuses `TextBox` for both history and message input.

History box:

```cpp
TextBox history_box(0, 0, false, make_textbox_rules(colors));
```

Message input:

```cpp
TextBox message_input(0, 92, true, make_textbox_rules(colors));
```

Send button:

```cpp
Button send_button("Send", 120, 92,
                   make_button_rules(colors,
                                     colors.primary,
                                     colors.on_primary,
                                     colors.secondary,
                                     colors.on_secondary));
send_button.callback(on_send_click);
```

## Step 7: Add networking

The framework already provides TCP / HTTP support behind `network.hpp`.

A chat app usually needs:

- a TCP connection
- send operations
- non-blocking receive polling

The `DuckChat` example uses `PollerComponent` to poll the socket once per frame:

```cpp
PollerComponent poller;
poller.set_update_proc(poll_chat_socket);
window.addComponent(poller);
```

This keeps the UI responsive without blocking the window loop.

## Step 8: Save JSON config

On first launch, save:

- host
- port
- username

Current config paths:

- Linux / Windows: `$HOME/.config/stardustui/chat.json`
- XJ380: `/etc/stardustui-chat.json`

You can directly study these functions in:

```text
examples/duckchat/duckchat.cpp
```

- `load_config`
- `save_config`
- `apply_setup_inputs`

## Step 9: Switch screens

If config already exists:

- go directly to the chat screen
- try to connect immediately

If config does not exist:

- show the setup screen

The logic is:

```cpp
if (g_config_loaded) {
    show_screen(&chat_screen);
    connect_chat();
} else {
    show_screen(&setup_screen);
}
```

## Step 10: Build and run

Linux:

```bash
cd examples/duckchat
make PLATFORM=linux
./build/linux/duckchat green_light
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

## Recommended implementation order

Build in this order:

1. create the window and both screens
2. add text boxes and buttons
3. add JSON config loading and saving
4. add TCP send / receive
5. finish theme integration, status text, and final copy

## Related files

- example source: `examples/duckchat/duckchat.cpp`
- example build: `examples/duckchat/Makefile`
- theme system: `src/theme.cpp`
- networking: `src/network.cpp`

