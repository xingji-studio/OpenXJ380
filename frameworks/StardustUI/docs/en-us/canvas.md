# Canvas Component

StardustUI currently provides a `Canvas` component for custom pixel-based drawing.

## Include header

```cpp
#include "../../includes/components/canvas.hpp"
```

## Purpose

Use `Canvas` when a built-in text component is not enough and you want to draw pixels or filled rectangles yourself.

This was also the component used by the older `layout` example to draw colored blocks for each flex region.

## API

```cpp
class Canvas : public base_component
```

Constructor:

```cpp
Canvas(int width, int height);
```

Drawing methods:

```cpp
void clear();
void set_pixel(int x, int y, unsigned int color);
void fill_rect(int x, int y, int width, int height, unsigned int color);
```

Refresh hook:

```cpp
using RefreshCallback = void (*)(Canvas&);
void set_refresh_callback(RefreshCallback callback);
```

## Refresh behavior

The refresh callback is called from `Canvas::update()`.

Because `Window::show()` calls `update()` on every component during the main loop, the canvas callback runs once per loop iteration.

The current flow is:

1. `Canvas::update()` clears the previous commands
2. the refresh callback rebuilds the current frame
3. the canvas requests a redraw
4. the window redraws all components

This makes `Canvas` suitable for simple animations and dynamic debug visuals.

## Coordinates

All canvas drawing coordinates are local to the canvas itself.

Example:

```cpp
canvas.set_pixel(2, 2, 0x000000FF);
canvas.fill_rect(0, 0, 100, 40, 0xE85D5DFF);
```

When the canvas is drawn, StardustUI offsets these commands by the canvas position in the window.

## Example

```cpp
void draw_header(Canvas& canvas) {
    canvas.fill_rect(0, 0, canvas.get_width(), canvas.get_height(), 0xE85D5DFF);
    canvas.set_pixel(2, 2, 0x000000FF);
}

Canvas header(0, 96);
header.set_refresh_callback(draw_header);
```

## Notes

- `Canvas` clips out-of-range pixels and rectangles.
- `Canvas` reports its preferred size from its current bounds.
- `Canvas` can be used directly in `Window`, or as a child of `FlexLayout`.

## Related pages

- [Layout System](./layout.md)
- [Create a Window](./create_window.md)
