# Button Component

`Button` is the most direct clickable component in StardustUI.

## Include header

```cpp
#include "../../includes/components/button.hpp"
```

## Class

```cpp
class Button : public base_component
```

## Constructors

```cpp
Button(const stardustui::string& text, int width, int height);
Button(const stardustui::string& text, int width, int height, const SytelRules& style);
```

Parameters:

- `text`: button label
- `width`: button width
- `height`: button height
- `style`: optional style rules

## Common methods

```cpp
void set_text(const stardustui::string& text);
const stardustui::string& get_text() const;
```

Click handling comes from `base_component`, and is usually wired like this:

```cpp
button.callback(on_button_click);
```

## Example

```cpp
Button send_button("Send", 120, 48,
                   make_button_rules(colors,
                                     colors.primary,
                                     colors.on_primary,
                                     colors.secondary,
                                     colors.on_secondary));
send_button.callback(on_send_click);
```

## How DuckChat uses it

`examples/duckchat/duckchat.cpp` uses three buttons:

- `Save And Connect`
- `Reconnect`
- `Send`

These are used to:

- save config and connect
- reconnect to the chat server
- send the current message

## Related pages

- [TextBox component](./textbox.md)
- [Style system](./style.md)
- [DuckChat tutorial](./duckchat_tutorial.md)
