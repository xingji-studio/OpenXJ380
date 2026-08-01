# ScrollBar Component

`ScrollBar` is a vertical scrollbar component intended for text areas or custom scrolling regions.

## Include header

```cpp
#include "../../includes/components/scrollbar.hpp"
```

## Class

```cpp
class ScrollBar : public base_component
```

## Constructors

```cpp
ScrollBar(int width, int height);
ScrollBar(int width, int height, const SytelRules& style);
```

## Common methods

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

## Behavior

The scrollbar computes its maximum value and thumb size from:

- `content_size`
- `page_size`

Example:

```cpp
scrollbar.set_range(2000, 480);
scrollbar.set_value(120);
```

This means:

- total content height is `2000`
- visible area height is `480`
- current scroll position is `120`

## Interaction

`ScrollBar` implements:

```cpp
bool handle_pointer_move(int x, int y) override;
bool handle_left_button(bool pressed, int x, int y) override;
```

It supports:

- clicking the track
- dragging the thumb

## How TextBox uses it

`TextBox` already embeds a `ScrollBar`, so for most text scrolling use cases you do not need to place one manually.

## Related pages

- [TextBox component](./textbox.md)
- [DuckChat tutorial](./duckchat_tutorial.md)
