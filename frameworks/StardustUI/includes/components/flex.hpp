#pragma once
#include "base.hpp"
#include "../vector.hpp"

class FlexLayout : public base_component
{
public:
    enum Direction {
        Row,
        Column
    };

    enum Align {
        AlignStart,
        AlignCenter,
        AlignEnd,
        AlignStretch
    };

    enum Justify {
        JustifyStart,
        JustifyCenter,
        JustifyEnd,
        JustifySpaceBetween
    };

    FlexLayout(int width, int height);
    ~FlexLayout() override;

    void draw(unsigned long long handle) override;
    void update() override;
    bool contains(int x, int y) const override;
    int get_preferred_width() const override;
    int get_preferred_height() const override;
    void set_bounds(int x, int y, int width, int height) override;
    bool handle_pointer_move(int x, int y) override;
    bool handle_left_button(bool pressed, int x, int y) override;
    bool handle_char_input(char ch, bool special) override;

    void set_direction(Direction direction);
    void set_align_items(Align align);
    void set_justify_content(Justify justify);
    void set_gap(int gap);
    void set_padding(int padding);

    void addComponent(base_component& component, int flex_grow = 0);
    void addComponent(base_component* component, int flex_grow = 0);

private:
    struct Item {
        base_component* component;
        int flex_grow;
        int basis_main;
        int basis_cross;

        Item() : component(nullptr), flex_grow(0), basis_main(0), basis_cross(0) {}
    };

    stardustui::vector<Item> items;
    Direction direction;
    Align align_items;
    Justify justify_content;
    int gap;
    int padding;
    bool layout_dirty;

    void perform_layout();
};
