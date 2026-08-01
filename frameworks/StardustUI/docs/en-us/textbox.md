# TextBox Component

`TextBox` is used for displaying and entering text.

It can work as a read-only text area or as an editable input box.

## Include header

```cpp
#include "../../includes/components/textbox.hpp"
```

## Class

```cpp
class TextBox : public base_component
```

## Constructors

```cpp
TextBox(int width, int height, bool input = true);
TextBox(int width, int height, bool input, const SytelRules& style);
```

Parameters:

- `width`: component width
- `height`: component height
- `input`: whether text input is enabled
- `style`: optional style rules

## Common methods

```cpp
void set_text(const stardustui::string& text);
const stardustui::string& get_text() const;
void set_input_enabled(bool enabled);
bool is_input_enabled() const;
bool set_focus(bool focused) override;
```

## Input-related overrides

`TextBox` implements:

```cpp
bool handle_pointer_move(int x, int y) override;
bool handle_left_button(bool pressed, int x, int y) override;
bool handle_char_input(char ch, bool special) override;
```

So it can:

- react to hover and click
- receive focus
- accept character input
- handle its internal scrollbar dragging

## Built-in scrollbar

`TextBox` owns an internal `ScrollBar` for long content.

This makes it useful for:

- chat history
- long text output
- scrollable input content

## Example

Read-only history box:

```cpp
TextBox history_box(0, 0, false, make_textbox_rules(colors));
```

Editable message box:

```cpp
TextBox message_input(0, 92, true, make_textbox_rules(colors));
```

Setting text:

```cpp
message_input.set_text("hello");
```

## How DuckChat uses it

`examples/duckchat/duckchat.cpp` uses `TextBox` for:

- host input
- port input
- username input
- chat history display
- message input

## Related pages

- [ScrollBar component](./scrollbar.md)
- [Button component](./button.md)
- [DuckChat tutorial](./duckchat_tutorial.md)
