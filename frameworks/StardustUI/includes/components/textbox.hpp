#pragma once
#include "base.hpp"
#include "scrollbar.hpp"
#include "../string.hpp"
#include "../vector.hpp"

class TextBox : public base_component
{
public:
    TextBox(int width, int height, bool input = true);
    TextBox(int width, int height, bool input, const SytelRules& style);
    ~TextBox() override;

    void draw(unsigned long long handle) override;
    void update() override;
    int get_preferred_width() const override;
    int get_preferred_height() const override;
    void set_bounds(int x, int y, int width, int height) override;
    bool handle_pointer_move(int x, int y) override;
    bool handle_left_button(bool pressed, int x, int y) override;
    bool handle_char_input(char ch, bool special) override;
    bool set_focus(bool focused) override;

    void set_text(const stardustui::string& text);
    const stardustui::string& get_text() const;
    void set_input_enabled(bool enabled);
    bool is_input_enabled() const;

private:
    struct WrappedLine {
        int start;
        int length;

        WrappedLine() : start(0), length(0) {}
    };

    stardustui::string text;
    stardustui::string scratch_text;
    bool input_enabled;
    bool cursor_visible;
    int cursor_blink_ticks;
    int scroll_offset_lines;
    bool layout_dirty;
    bool ensure_cursor_visible_pending;
    unsigned int cached_char_width_size;
    bool cached_char_width_valid[256];
    unsigned int cached_char_widths[256];
    ScrollBar scrollbar;
    stardustui::vector<WrappedLine> wrapped_lines;

    void reset_cursor_blink();
    void invalidate_layout();
    int get_line_height(unsigned int text_size) const;
    int get_scrollbar_width() const;
    void rebuild_wrapped_lines(unsigned int text_size, int content_width);
    void update_layout();
    void sync_scroll_offset(int visible_lines);
    bool erase_last_character();
    stardustui::string build_text_slice(int start, int length) const;
    int get_cached_char_width(unsigned int text_size, unsigned char ch);
};
