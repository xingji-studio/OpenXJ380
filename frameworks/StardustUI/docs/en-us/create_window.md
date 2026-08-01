# Create a Window

This page describes the current `Window` API in StardustUI.

## Include headers

```cpp
#include "../../includes/window.hpp"
#include "../../includes/components/lable.hpp"
```

## Minimal example

```cpp
Window window("Hello, World!", 400, 300);

Lable hello_label("Hello, World!", 24, 0x000000FF);
hello_label.set_pos(100, 100);

window.addComponent(hello_label);
window.show();
```

## Constructor

```cpp
Window(const char* title, int width, int height);
```

| Argument | Meaning |
| --- | --- |
| `title` | Window title |
| `width` | Window width |
| `height` | Window height |

Example:

```cpp
Window window("Demo", 800, 600);
```

## Main methods

```cpp
void show();
void hide();
int getWidth();
int getHeight();
const char* getTitle();
void error(const char* msg);
```

- `show()` creates the native window, enters the event loop, and keeps running until the window is closed.
- `hide()` closes the current native window handle if it exists.
- `getWidth()`, `getHeight()`, and `getTitle()` return the values passed to the constructor.
- `error()` forwards an error message to the current platform backend.
- During the main loop, `Window::show()` also calls `component->update()` on every component once per frame.

## Add components

StardustUI currently exposes two overloads:

```cpp
void addComponent(base_component& component);
void addComponent(base_component* component);
```

Both overloads store a pointer internally. That means the component object must stay alive for the whole lifetime of the window loop.

Safe pattern:

```cpp
Window window("Demo", 400, 300);
Lable label("Text", 24, 0x000000FF);

window.addComponent(label);
window.show();
```

Unsafe pattern:

```cpp
Window window("Demo", 400, 300);
window.addComponent(Lable("Text", 24, 0x000000FF)); // temporary object
```

## Mouse and hover behavior

`Window::show()` already runs the platform event loop and updates component hover state through `handle_message(...)`.

For components that implement `contains(int x, int y)`, hover style changes will be applied automatically when the mouse moves over that component.

## Redraw behavior

Components can request a redraw from inside `update()` or other state-changing methods.

This is how dynamic components such as `Canvas` refresh every frame without the user manually forcing a window repaint.

## Related pages

- [Quick Start](./quickstart.md)
- [Style System](./style.md)
- [Layout System](./layout.md)
- [Canvas Component](./canvas.md)
- [Documentation Index](./docs.md)
