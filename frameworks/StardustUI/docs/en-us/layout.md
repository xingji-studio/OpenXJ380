# Layout System

StardustUI currently provides a flex-style container component named `FlexLayout`.

## Include header

```cpp
#include "../../includes/components/flex.hpp"
```

## Purpose

`FlexLayout` arranges child components in either a row or a column, and can combine fixed-size items with flexible items that grow to fill the remaining space.

## API

```cpp
class FlexLayout : public base_component
```

Constructor:

```cpp
FlexLayout(int width, int height);
```

Configuration:

```cpp
void set_direction(Direction direction);
void set_align_items(Align align);
void set_justify_content(Justify justify);
void set_gap(int gap);
void set_padding(int padding);
```

Add children:

```cpp
void addComponent(base_component& component, int flex_grow = 0);
void addComponent(base_component* component, int flex_grow = 0);
```

## Directions

```cpp
FlexLayout::Row
FlexLayout::Column
```

- `Row`: children are laid out from left to right
- `Column`: children are laid out from top to bottom

## Align modes

```cpp
FlexLayout::AlignStart
FlexLayout::AlignCenter
FlexLayout::AlignEnd
FlexLayout::AlignStretch
```

These control the cross axis:

- in a row layout, this affects vertical placement
- in a column layout, this affects horizontal placement

## Justify modes

```cpp
FlexLayout::JustifyStart
FlexLayout::JustifyCenter
FlexLayout::JustifyEnd
FlexLayout::JustifySpaceBetween
```

These control the main axis:

- in a row layout, this affects horizontal placement
- in a column layout, this affects vertical placement

## `flex_grow`

When you add a child, you can pass `flex_grow`.

Example:

```cpp
content.addComponent(sidebar, 0);
content.addComponent(main_column, 1);
```

Meaning:

- `sidebar` keeps its preferred size
- `main_column` grows to fill the remaining space

## Preferred size

`FlexLayout` uses each child's preferred size:

```cpp
virtual int get_preferred_width() const;
virtual int get_preferred_height() const;
```

For example:

- `Lable` reports preferred size from text width and resolved text size
- `Canvas` reports preferred size from its current bounds

## Example

This is the current layout-related example path:

```text
examples/duckchat/duckchat.cpp
```

The `DuckChat` example builds a structure like this:

1. root column
2. header
3. content row
4. sidebar + main column
5. main canvas + bottom row
6. two bottom canvases

## Example snippet

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

## Notes

- `FlexLayout` is itself a component, so it can be added directly to `Window`.
- Nested layouts are supported.
- Child `update()` methods are called from the layout during the window loop.
- If a child requests redraw, the layout propagates that redraw request upward.

## Related pages

- [Canvas Component](./canvas.md)
- [Create a Window](./create_window.md)
- [Quick Start](./quickstart.md)
- [DuckChat tutorial](./duckchat_tutorial.md)
