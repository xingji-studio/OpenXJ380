#pragma once
#include "base.hpp"

class ScrollBar : public base_component
{
public:
    ScrollBar(int width, int height);
    ScrollBar(int width, int height, const SytelRules& style);
    ~ScrollBar() override;

    void draw(unsigned long long handle) override;
    void update() override;
    int get_preferred_width() const override;
    int get_preferred_height() const override;
    bool handle_pointer_move(int x, int y) override;
    bool handle_left_button(bool pressed, int x, int y) override;

    void set_range(int content_size, int page_size);
    void set_value(int value);
    int get_value() const;
    int get_content_size() const;
    int get_page_size() const;
    int get_max_value() const;
    bool is_dragging() const;

    void set_change_callback(void (*func)(ScrollBar&, int));

private:
    int content_size;
    int page_size;
    int value;
    bool dragging;
    int drag_anchor_y;
    int drag_anchor_value;
    void (*change_callback_func)(ScrollBar&, int);

    int get_track_x() const;
    int get_track_y() const;
    int get_track_width() const;
    int get_track_height() const;
    int get_thumb_height() const;
    int get_thumb_offset() const;
    int get_max_thumb_offset() const;
    bool set_value_internal(int value, bool notify);
};
