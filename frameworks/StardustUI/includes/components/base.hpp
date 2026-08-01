#pragma once
#include "../../settings.hpp"
#include "../../platforms/platform.hpp"
#include "../sytle.hpp"
class base_component
{
public:
    enum Anchor {
        AnchorLeft = 1 << 0,
        AnchorTop = 1 << 1,
        AnchorRight = 1 << 2,
        AnchorBottom = 1 << 3
    };

    base_component();
    virtual ~base_component();
    base_component(const base_component&) = delete;
    base_component& operator=(const base_component&) = delete;
    base_component(base_component&&) = delete;
    base_component& operator=(base_component&&) = delete;
    virtual void draw(unsigned long long handle);
    virtual void update();
    virtual void callback(void (*func)());
    void set_style_rules(const SytelRules& rules);
    const SytelRules& get_style_rules() const;
    void clear_style_rules();
    void set_mouse_state(bool active);
    void set_click_state(bool active);
    void set_hover_state(bool active);
    bool is_mouse_active() const;
    bool is_click_active() const;
    bool is_hover_active() const;
    Sytel resolve_style() const;
    virtual int get_preferred_width() const;
    virtual int get_preferred_height() const;
    virtual bool contains(int x, int y) const;
    virtual void set_bounds(int x, int y, int width, int height);
    virtual bool handle_pointer_move(int x, int y);
    virtual bool handle_left_button(bool pressed, int x, int y);
    virtual bool handle_char_input(char ch, bool special);
    virtual bool set_focus(bool focused);
    virtual void on_parent_resize(int old_width, int old_height, int new_width, int new_height);
    bool has_focus() const;
    int get_width() const;
    int get_height() const;
    void set_anchors(unsigned int anchors);
    unsigned int get_anchors() const;
    void request_redraw();
    bool consume_redraw_request();
    bool has_pending_redraw() const;
    void run_callback();
    void set_pos(int x,int y){
        this->x=x;
        this->y=y;
    }
    void get_pos(int &x,int &y) const {
        x=this->x;
        y=this->y;
    }
protected:
    void (*callback_func)() = nullptr;   
    SytelRules style_rules;
    bool mouse_active;
    bool click_active;
    bool hover_active;
    bool focused;
    bool redraw_requested;
    int click_feedback_frames;
    unsigned int anchors;
    unsigned int x,y;
    unsigned int width,height;
};
