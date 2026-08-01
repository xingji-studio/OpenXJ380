# Style System

StardustUI's current style system is built around two classes:

- `Sytel`
- `SytelRules`

The names are intentionally written exactly as they appear in the codebase.

## Include header

```cpp
#include "../../includes/sytle.hpp"
```

## `Sytel`

`Sytel` is a single style block. Each field is optional.

### Supported properties

```cpp
void set_color(unsigned int color);
void set_size(unsigned int size);
void set_background_color(unsigned int color);
void set_border_color(unsigned int color);
void set_border_width(unsigned int width);
void set_radius(unsigned int radius);
void set_padding(unsigned int padding);
```

Each property also has:

- `unset_*()`
- `has_*() const`
- `get_*(fallback) const`

There are also utility methods:

```cpp
void clear();
void merge_from(const Sytel& sytel);
bool empty() const;
```

## `SytelRules`

`SytelRules` stores style blocks for different states:

```cpp
void set_base_sytel(const Sytel& sytel);
void set_on_mouse_sytel(const Sytel& sytel);
void set_on_click_sytel(const Sytel& sytel);
void set_on_hover_sytel(const Sytel& sytel);
```

Resolved style order is:

1. base
2. on mouse
3. on click
4. on hover

The merge happens in `SytelRules::resolve(bool on_mouse, bool on_click, bool on_hover)`.

## Apply styles to a component

All components derive from `base_component`, which provides:

```cpp
void set_style_rules(const SytelRules& rules);
const SytelRules& get_style_rules() const;
void clear_style_rules();
Sytel resolve_style() const;
```

## Hover example

This is the current `helloworld` pattern:

```cpp
Window window("Hello, World!", 400, 300);

Sytel base_style;
base_style.set_color(0x000000FF);
base_style.set_size(24);

Sytel hover_style;
hover_style.set_color(0xFF0000FF);

SytelRules rules;
rules.set_base_sytel(base_style);
rules.set_on_hover_sytel(hover_style);

Lable hello_label("Hello, World!", 24, 0x000000FF);
hello_label.set_style_rules(rules);
hello_label.set_pos(100, 100);

window.addComponent(hello_label);
window.show();
```

Result:

- normal text color: black
- hover text color: red

## Notes about text size

For text components such as `Lable`, the final size is resolved like this:

```cpp
resolved_size = style.get_size(constructor_size);
```

So:

- the constructor provides the default size
- `Sytel::set_size(...)` overrides it for the active state

The exact final visual size depends on the backend implementation for the current platform.

## Current component support

The style system is already wired into:

- `base_component`
- `Lable`
- `Canvas`
- `FlexLayout`

`Lable::draw(...)` currently reads:

- text color
- text size

`Lable::contains(...)` also uses the resolved text size for hover hit testing.

`Canvas` and `FlexLayout` inherit the same base style and redraw mechanism, but they do not currently render borders, background color, radius, or padding automatically from `Sytel`. Those values remain available for higher-level components and future expansion.

## Related pages

- [Create a Window](./create_window.md)
- [Quick Start](./quickstart.md)
- [Layout System](./layout.md)
- [Canvas Component](./canvas.md)
