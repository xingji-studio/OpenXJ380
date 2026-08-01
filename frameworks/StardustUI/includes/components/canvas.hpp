#pragma once
#include "base.hpp"
#include "../vector.hpp"

class Canvas : public base_component
{
public:
    using RefreshCallback = void (*)(Canvas&);

    Canvas(int width, int height);
    ~Canvas() override;

    void draw(unsigned long long handle) override;
    void update() override;
    bool contains(int x, int y) const override;
    int get_preferred_width() const override;
    int get_preferred_height() const override;
    void set_bounds(int x, int y, int width, int height) override;

    void set_refresh_callback(RefreshCallback callback);
    void clear();
    void set_pixel(int x, int y, unsigned int color);
    void fill_rect(int x, int y, int width, int height, unsigned int color);

private:
    struct Command {
        enum Type {
            Pixel,
            Rect
        };

        Type type;
        int x;
        int y;
        int width;
        int height;
        unsigned int color;

        Command() : type(Pixel), x(0), y(0), width(0), height(0), color(0) {}
    };

    stardustui::vector<Command> commands;
    RefreshCallback refresh_callback;
};
