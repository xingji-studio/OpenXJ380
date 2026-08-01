#include "../../includes/window.hpp"
#include "../../includes/components/button.hpp"
#include "../../includes/components/lable.hpp"
#include "../../includes/components/scrollbar.hpp"
#include "../../includes/components/textbox.hpp"
#include "../../includes/sytle.hpp"

namespace {
Lable* g_status_label = nullptr;
ScrollBar* g_demo_scrollbar = nullptr;

void on_button_click() {
    if (g_status_label != nullptr) {
        g_status_label->set_text("Button clicked!");
    }
}

void on_scroll_changed(ScrollBar&, int) {
    if (g_status_label == nullptr || g_demo_scrollbar == nullptr) {
        return;
    }

    stardustui::string text("Scrollbar value: ");
    const int value = g_demo_scrollbar->get_value();
    if (value >= 10) {
        text.push_char(static_cast<char>('0' + (value / 10) % 10));
    }
    text.push_char(static_cast<char>('0' + value % 10));
    g_status_label->set_text(text);
}
}

static int helloworld_main_impl(int, char**, char**) {
    append_debug_log("helloworld: main enter\n");
    Window window("Hello, World!", 560, 360);
    append_debug_log("helloworld: window constructed\n");

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
    hello_label.set_pos(32, 28);

    Lable status_label("Click the button or drag the scrollbar", 16, 0x444444FF);
    status_label.set_pos(32, 322);
    g_status_label = &status_label;

    Sytel button_base;
    button_base.set_color(0x000000FF);
    button_base.set_size(16);
    button_base.set_background_color(0xDCDCDCFF);
    button_base.set_border_color(0x707070FF);
    button_base.set_border_width(1);
    button_base.set_padding(12);

    Sytel button_hover;
    button_hover.set_background_color(0xFFB347FF);

    Sytel button_click;
    button_click.set_background_color(0xFF8C42FF);
    button_click.set_color(0xFFFFFFFF);

    SytelRules button_rules;
    button_rules.set_base_sytel(button_base);
    button_rules.set_on_hover_sytel(button_hover);
    button_rules.set_on_click_sytel(button_click);

    Button button("Click Me", 160, 48, button_rules);
    button.set_pos(32, 72);
    button.callback(on_button_click);

    Sytel textbox_base;
    textbox_base.set_color(0x202020FF);
    textbox_base.set_size(16);
    textbox_base.set_background_color(0xFFFFFFFF);
    textbox_base.set_border_color(0x9A9A9AFF);
    textbox_base.set_border_width(1);
    textbox_base.set_padding(8);

    Sytel textbox_hover;
    textbox_hover.set_border_color(0xFF6A00FF);

    Sytel textbox_click;
    textbox_click.set_border_color(0x4A90E2FF);

    SytelRules textbox_rules;
    textbox_rules.set_base_sytel(textbox_base);
    textbox_rules.set_on_hover_sytel(textbox_hover);
    textbox_rules.set_on_click_sytel(textbox_click);

    TextBox textbox(320, 180, true, textbox_rules);
    textbox.set_pos(208, 72);
    textbox.set_text("This textbox now supports multiline input. Type here, press Enter for a new line, and keep typing until the internal scrollbar appears.");

    Sytel scrollbar_base;
    scrollbar_base.set_background_color(0xF2F2F2FF);
    scrollbar_base.set_border_color(0xA8A8A8FF);
    scrollbar_base.set_border_width(1);
    scrollbar_base.set_color(0x737373FF);

    Sytel scrollbar_hover;
    scrollbar_hover.set_color(0xFF6A00FF);

    Sytel scrollbar_click;
    scrollbar_click.set_color(0x4A90E2FF);

    SytelRules scrollbar_rules;
    scrollbar_rules.set_base_sytel(scrollbar_base);
    scrollbar_rules.set_on_hover_sytel(scrollbar_hover);
    scrollbar_rules.set_on_click_sytel(scrollbar_click);

    ScrollBar demo_scrollbar(16, 180, scrollbar_rules);
    demo_scrollbar.set_pos(536, 72);
    demo_scrollbar.set_range(40, 10);
    demo_scrollbar.set_change_callback(on_scroll_changed);
    g_demo_scrollbar = &demo_scrollbar;

    window.addComponent(hello_label);
    window.addComponent(button);
    window.addComponent(textbox);
    window.addComponent(demo_scrollbar);
    window.addComponent(status_label);
    window.show();
    append_debug_log("helloworld: window.show returned\n");
    return 0;
}

#if defined(STARDUSTUI_WINDOWS) || defined(STARDUSTUI_LINUX)
int main(int argc, char *argv[], char *envp[])
{
    return helloworld_main_impl(argc, argv, envp);
}
#else
extern "C" int helloworld_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int helloworld_main_cpp(int argc, char *argv[], char *envp[])
{
    return helloworld_main_impl(argc, argv, envp);
}
#endif
