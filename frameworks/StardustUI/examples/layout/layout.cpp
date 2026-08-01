#include "../../includes/window.hpp"
#include "../../includes/components/canvas.hpp"
#include "../../includes/components/flex.hpp"

namespace {
unsigned int g_animation_tick = 0;

void draw_canvas_block(Canvas& canvas, unsigned int base_color, bool animate_vertical) {
    canvas.fill_rect(0, 0, canvas.get_width(), canvas.get_height(), base_color);

    const int stripe_size = animate_vertical ? 10 : 8;
    if (animate_vertical) {
        const int span = canvas.get_width() <= 0 ? 1 : canvas.get_width();
        const int stripe_x = static_cast<int>(g_animation_tick % static_cast<unsigned int>(span));
        canvas.fill_rect(stripe_x, 0, stripe_size, canvas.get_height(), 0xFFFFFFFF);
    } else {
        const int span = canvas.get_height() <= 0 ? 1 : canvas.get_height();
        const int stripe_y = static_cast<int>(g_animation_tick % static_cast<unsigned int>(span));
        canvas.fill_rect(0, stripe_y, canvas.get_width(), stripe_size, 0xFFFFFFFF);
    }

    canvas.set_pixel(2, 2, 0x000000FF);
}

void draw_header(Canvas& canvas) {
    ++g_animation_tick;
    draw_canvas_block(canvas, 0xE85D5DFF, true);
}

void draw_sidebar(Canvas& canvas) {
    draw_canvas_block(canvas, 0x57B36EFF, false);
}

void draw_main(Canvas& canvas) {
    draw_canvas_block(canvas, 0x4A90E2FF, true);
}

void draw_bottom_left(Canvas& canvas) {
    draw_canvas_block(canvas, 0xF2B84BFF, false);
}

void draw_bottom_right(Canvas& canvas) {
    draw_canvas_block(canvas, 0x8B6FE8FF, true);
}
}

static int layout_main_impl(int, char**, char**) {
    Window window("StardustUI Layout Demo", 900, 600);

    FlexLayout root(860, 560);
    root.set_pos(20, 20);
    root.set_direction(FlexLayout::Column);
    root.set_gap(16);
    root.set_padding(16);

    Canvas header(0, 96);
    header.set_refresh_callback(draw_header);

    FlexLayout content(0, 0);
    content.set_direction(FlexLayout::Row);
    content.set_gap(16);

    Canvas sidebar(180, 0);
    sidebar.set_refresh_callback(draw_sidebar);

    FlexLayout main_column(0, 0);
    main_column.set_direction(FlexLayout::Column);
    main_column.set_gap(16);

    Canvas main_canvas(0, 0);
    main_canvas.set_refresh_callback(draw_main);

    FlexLayout bottom_row(0, 180);
    bottom_row.set_direction(FlexLayout::Row);
    bottom_row.set_gap(16);

    Canvas bottom_left(0, 0);
    bottom_left.set_refresh_callback(draw_bottom_left);

    Canvas bottom_right(0, 0);
    bottom_right.set_refresh_callback(draw_bottom_right);

    bottom_row.addComponent(bottom_left, 1);
    bottom_row.addComponent(bottom_right, 1);

    main_column.addComponent(main_canvas, 1);
    main_column.addComponent(bottom_row, 0);

    content.addComponent(sidebar, 0);
    content.addComponent(main_column, 1);

    root.addComponent(header, 0);
    root.addComponent(content, 1);

    window.addComponent(root);
    window.show();
    return 0;
}

#if defined(STARDUSTUI_WINDOWS) || defined(STARDUSTUI_LINUX)
int main(int argc, char *argv[], char *envp[])
{
    return layout_main_impl(argc, argv, envp);
}
#else
extern "C" int layout_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int layout_main_cpp(int argc, char *argv[], char *envp[])
{
    return layout_main_impl(argc, argv, envp);
}
#endif
