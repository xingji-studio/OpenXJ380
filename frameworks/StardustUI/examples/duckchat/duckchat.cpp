#include "../../includes/window.hpp"
#include "../../includes/components/button.hpp"
#include "../../includes/components/flex.hpp"
#include "../../includes/components/lable.hpp"
#include "../../includes/components/textbox.hpp"
#include "../../includes/file.hpp"
#include "../../includes/json.hpp"
#include "../../includes/network.hpp"
#include "../../includes/sytle.hpp"
#include "../../includes/theme.hpp"
#include "../../platforms/platform.hpp"
#ifdef XJ380
#include "../../platforms/xj380/xapi/xtuiapi.h"
#include "../../platforms/xj380/xapi/xposix/stdio.h"
#endif

#if defined(STARDUSTUI_LINUX) || defined(STARDUSTUI_WINDOWS)
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(STARDUSTUI_WINDOWS)
#include <direct.h>
#endif
#endif

namespace {

struct AppConfig {
    stardustui::string username;
    stardustui::string host;
    unsigned short port;

    AppConfig() : username("User"), host(""), port(8888) {}
};

class PollerComponent : public base_component {
public:
    using UpdateProc = void (*)();

    PollerComponent() : proc(nullptr) {}

    void set_update_proc(UpdateProc update_proc) {
        this->proc = update_proc;
    }

    void update() override {
        base_component::update();
        if (this->proc != nullptr) {
            this->proc();
        }
    }

    bool contains(int, int) const override {
        return false;
    }

private:
    UpdateProc proc;
};

class ScreenHost : public base_component {
public:
    ScreenHost() : active(nullptr) {}

    void set_active(base_component* component) {
        this->active = component;
        sync_active_bounds();
        request_redraw();
    }

    void draw(unsigned long long handle) override {
        if (this->active != nullptr) {
            this->active->draw(handle);
        }
    }

    void update() override {
        base_component::update();
        if (this->active != nullptr) {
            this->active->update();
            if (this->active->consume_redraw_request()) {
                request_redraw();
            }
        }
    }

    bool contains(int x, int y) const override {
        return this->active != nullptr && this->active->contains(x, y);
    }

    void set_bounds(int x, int y, int width, int height) override {
        base_component::set_bounds(x, y, width, height);
        sync_active_bounds();
    }

    bool handle_pointer_move(int x, int y) override {
        return this->active != nullptr && this->active->handle_pointer_move(x, y);
    }

    bool handle_left_button(bool pressed, int x, int y) override {
        return this->active != nullptr && this->active->handle_left_button(pressed, x, y);
    }

    bool handle_char_input(char ch, bool special) override {
        return this->active != nullptr && this->active->handle_char_input(ch, special);
    }

private:
    void sync_active_bounds() {
        if (this->active != nullptr) {
            this->active->set_bounds(static_cast<int>(this->x),
                                     static_cast<int>(this->y),
                                     static_cast<int>(this->width),
                                     static_cast<int>(this->height));
        }
    }

    base_component* active;
};

Window* g_window = nullptr;
ScreenHost* g_screen_host = nullptr;
FlexLayout* g_setup_screen = nullptr;
FlexLayout* g_chat_screen = nullptr;
TextBox* g_host_input = nullptr;
TextBox* g_port_input = nullptr;
TextBox* g_username_input = nullptr;
TextBox* g_history_box = nullptr;
TextBox* g_message_input = nullptr;
Lable* g_status_label = nullptr;
Lable* g_room_meta_label = nullptr;
Lable* g_setup_hint_label = nullptr;
Button* g_send_button = nullptr;
Button* g_connect_button = nullptr;
Button* g_reconnect_button = nullptr;
PollerComponent* g_poller = nullptr;
stardustui::Socket g_chat_socket;
AppConfig g_config;
bool g_config_loaded = false;
bool g_connect_in_progress = false;
bool g_chat_connected = false;
stardustui::string g_history_text;

bool is_digit_text(const stardustui::string& text)
{
    if (text.length() <= 0) {
        return false;
    }

    const char* raw = text.c_str();
    for (int index = 0; raw[index] != '\0'; ++index) {
        if (raw[index] < '0' || raw[index] > '9') {
            return false;
        }
    }
    return true;
}

bool parse_port_text(const stardustui::string& text, unsigned short& out_port)
{
    out_port = 0;
    if (!is_digit_text(text)) {
        return false;
    }

    unsigned int value = 0;
    const char* raw = text.c_str();
    for (int index = 0; raw[index] != '\0'; ++index) {
        value = value * 10u + static_cast<unsigned int>(raw[index] - '0');
        if (value > 65535u) {
            return false;
        }
    }

    if (value == 0u) {
        return false;
    }

    out_port = static_cast<unsigned short>(value);
    return true;
}

void set_label_text(Lable* label, const char* text)
{
    if (label == nullptr) {
        return;
    }

    stardustui::string next;
    next.assign(text);
    label->set_text(next);
}

void set_label_text(Lable* label, const stardustui::string& text)
{
    if (label == nullptr) {
        return;
    }

    label->set_text(text);
}

void set_textbox_text(TextBox* textbox, const char* text)
{
    if (textbox == nullptr) {
        return;
    }

    stardustui::string next;
    next.assign(text);
    textbox->set_text(next);
}

stardustui::string config_path()
{
#ifdef XJ380
    return stardustui::string("/etc/stardustui-chat.json");
#else
    stardustui::string home;
    const char* raw_home = std::getenv("HOME");
#if defined(STARDUSTUI_WINDOWS)
    if ((raw_home == nullptr || raw_home[0] == '\0')) {
        raw_home = std::getenv("USERPROFILE");
    }
#endif
    if (raw_home == nullptr || raw_home[0] == '\0') {
        return stardustui::string("stardustui-chat.json");
    }

    home.assign(raw_home);
    home.append("/.config/stardustui/chat.json");
    return home;
#endif
}

bool ensure_config_directory_exists(const stardustui::string& path)
{
#ifdef XJ380
    const char* separators[] = {"/etc", "/etc/stardustui"};
    for (int index = 0; index < 2; ++index) {
        unsigned long long result = xapi_Mkdir((char*)separators[index]);
        if (!(result == 0 || result == 1 || result == 2)) {
            (void)result;
        }
    }
    return true;
#else
    const char* raw = path.c_str();
    int last_separator = -1;
    for (int index = 0; raw[index] != '\0'; ++index) {
        if (raw[index] == '/' || raw[index] == '\\') {
            last_separator = index;
        }
    }

    if (last_separator <= 0) {
        return true;
    }

    char partial[1024];
    int partial_length = 0;
    for (int index = 0; index < last_separator && partial_length + 1 < static_cast<int>(sizeof(partial)); ++index) {
        partial[partial_length++] = raw[index];
        partial[partial_length] = '\0';
        if (raw[index] != '/' && raw[index] != '\\') {
            continue;
        }

        if (partial_length <= 1) {
            continue;
        }

#if defined(STARDUSTUI_WINDOWS)
        _mkdir(partial);
#else
        mkdir(partial, 0755);
#endif
    }

    if (partial_length > 0 && partial[partial_length - 1] != '/' && partial[partial_length - 1] != '\\') {
#if defined(STARDUSTUI_WINDOWS)
        _mkdir(partial);
#else
        mkdir(partial, 0755);
#endif
    }
    return true;
#endif
}

bool save_config(const AppConfig& config)
{
    stardustui::string path = config_path();
    ensure_config_directory_exists(path);

    rapidjson::Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType& allocator = document.GetAllocator();

    rapidjson::Value username_value;
    username_value.SetString(config.username.c_str(),
                             static_cast<rapidjson::SizeType>(config.username.length()),
                             allocator);
    document.AddMember("username", username_value, allocator);

    rapidjson::Value host_value;
    host_value.SetString(config.host.c_str(),
                         static_cast<rapidjson::SizeType>(config.host.length()),
                         allocator);
    document.AddMember("host", host_value, allocator);
    document.AddMember("port", static_cast<unsigned int>(config.port), allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);

    return stardustui::File::write_text(path, buffer.GetString());
}

bool load_config(AppConfig& out_config)
{
    out_config = AppConfig();
    stardustui::string path = config_path();
    if (!stardustui::File::exists(path)) {
        return false;
    }

    stardustui::string json_text;
    if (!stardustui::File::read_text(path, json_text)) {
        return false;
    }

    rapidjson::Document document;
    document.Parse(json_text.c_str());
    if (document.HasParseError() || !document.IsObject()) {
        return false;
    }

    if (document.HasMember("username") && document["username"].IsString()) {
        out_config.username.assign(document["username"].GetString());
    }
    if (document.HasMember("host") && document["host"].IsString()) {
        out_config.host.assign(document["host"].GetString());
    }
    if (document.HasMember("port") && document["port"].IsUint()) {
        const unsigned int port_value = document["port"].GetUint();
        if (port_value > 0 && port_value <= 65535u) {
            out_config.port = static_cast<unsigned short>(port_value);
        }
    }

    return out_config.host.length() > 0;
}

void append_history_line(const char* text)
{
    if (text == nullptr) {
        return;
    }

    if (g_history_text.length() > 0) {
        g_history_text.push_char('\n');
    }
    g_history_text.append(text);

    if (g_history_box != nullptr) {
        g_history_box->set_text(g_history_text);
    }
}

void append_history_line(const stardustui::string& text)
{
    append_history_line(text.c_str());
}

void update_connection_status()
{
    if (g_status_label == nullptr || g_room_meta_label == nullptr) {
        return;
    }

    if (g_chat_connected) {
        stardustui::string status("Connected to ");
        status.append(g_config.host.c_str());
        status.append(":");
        char port_buffer[16];
        snprintf(port_buffer, sizeof(port_buffer), "%u", static_cast<unsigned int>(g_config.port));
        status.append(port_buffer);
        set_label_text(g_status_label, status);

        stardustui::string meta("Logged in as ");
        meta.append(g_config.username.c_str());
        set_label_text(g_room_meta_label, meta);
        return;
    }

    if (g_connect_in_progress) {
        set_label_text(g_status_label, "Connecting...");
        set_label_text(g_room_meta_label, "Attempting to reach server");
        return;
    }

    set_label_text(g_status_label, "Disconnected");
    set_label_text(g_room_meta_label, "Press reconnect after editing settings");
}

void show_screen(FlexLayout* active_screen)
{
    if (g_screen_host == nullptr || active_screen == nullptr) {
        return;
    }
    g_screen_host->set_active(active_screen);
}

void disconnect_chat()
{
    g_chat_socket.close();
    g_chat_connected = false;
    g_connect_in_progress = false;
    update_connection_status();
}

bool connect_chat()
{
    disconnect_chat();
    g_connect_in_progress = true;
    update_connection_status();

    if (!g_chat_socket.connect(g_config.host, g_config.port)) {
        g_connect_in_progress = false;
        g_chat_connected = false;
        update_connection_status();
        append_history_line("[System] Connection failed.");
        return false;
    }

    g_connect_in_progress = false;
    g_chat_connected = true;
    update_connection_status();

    stardustui::string connected_line("[System] Connected to ");
    connected_line.append(g_config.host.c_str());
    connected_line.append(":");
    char port_buffer[16];
    snprintf(port_buffer, sizeof(port_buffer), "%u", static_cast<unsigned int>(g_config.port));
    connected_line.append(port_buffer);
    append_history_line(connected_line);
    return true;
}

void poll_chat_socket()
{
    if (!g_chat_connected) {
        return;
    }

    unsigned char buffer[1024];
    int received = 0;
    if (!g_chat_socket.receive(buffer, static_cast<int>(sizeof(buffer) - 1), received)) {
        append_history_line("[System] Socket error.");
        disconnect_chat();
        return;
    }

    if (received <= 0) {
        return;
    }

    buffer[received] = '\0';
    append_history_line(reinterpret_cast<const char*>(buffer));
}

void on_send_click()
{
    if (g_message_input == nullptr) {
        return;
    }

    const stardustui::string message = g_message_input->get_text();
    if (message.length() <= 0) {
        set_label_text(g_status_label, "Type a message first");
        return;
    }

    if (!g_chat_connected) {
        set_label_text(g_status_label, "Not connected");
        return;
    }

    stardustui::string payload;
    payload.append(g_config.username.c_str());
    payload.append(": ");
    payload.append(message.c_str());

    if (!g_chat_socket.send_text(payload)) {
        append_history_line("[System] Send failed.");
        disconnect_chat();
        return;
    }

    append_history_line(payload);
    set_textbox_text(g_message_input, "");
    set_label_text(g_status_label, "Message sent");
}

void on_reconnect_click()
{
    connect_chat();
}

bool apply_setup_inputs()
{
    if (g_host_input == nullptr || g_port_input == nullptr || g_username_input == nullptr) {
        return false;
    }

    AppConfig next_config;
    next_config.host = g_host_input->get_text();
    next_config.username = g_username_input->get_text();
    if (next_config.host.length() <= 0) {
        set_label_text(g_setup_hint_label, "Server IP/domain is required");
        return false;
    }
    if (next_config.username.length() <= 0) {
        set_label_text(g_setup_hint_label, "Username is required");
        return false;
    }
    if (!parse_port_text(g_port_input->get_text(), next_config.port)) {
        set_label_text(g_setup_hint_label, "Port must be 1-65535");
        return false;
    }

    g_config = next_config;
    g_config_loaded = true;
    save_config(g_config);
    return true;
}

void on_connect_click()
{
    if (!apply_setup_inputs()) {
        return;
    }

    g_history_text.assign("");
    append_history_line("[System] Chat demo ready.");
    show_screen(g_chat_screen);
    connect_chat();
}

SytelRules make_panel_rules(unsigned int background,
                            unsigned int border,
                            unsigned int border_width = 1,
                            unsigned int radius = 24)
{
    Sytel panel;
    panel.set_background_color(background);
    panel.set_border_color(border);
    panel.set_border_width(border_width);
    panel.set_radius(radius);

    SytelRules rules;
    rules.set_base_sytel(panel);
    return rules;
}

SytelRules make_button_rules(const stardustui::Colors& colors,
                             unsigned int background,
                             unsigned int foreground,
                             unsigned int hover_background,
                             unsigned int hover_foreground)
{
    Sytel base;
    base.set_color(foreground);
    base.set_size(16);
    base.set_background_color(background);
    base.set_border_color(background);
    base.set_border_width(1);
    base.set_padding(12);
    base.set_radius(24);

    Sytel hover;
    hover.set_background_color(hover_background);
    hover.set_border_color(hover_background);
    hover.set_color(hover_foreground);

    Sytel click;
    click.set_background_color(colors.tertiary);
    click.set_border_color(colors.tertiary);
    click.set_color(colors.on_tertiary);

    SytelRules rules;
    rules.set_base_sytel(base);
    rules.set_on_hover_sytel(hover);
    rules.set_on_click_sytel(click);
    return rules;
}

SytelRules make_textbox_rules(const stardustui::Colors& colors)
{
    Sytel base;
    base.set_color(colors.on_surface);
    base.set_size(16);
    base.set_background_color(colors.surface);
    base.set_border_color(colors.outline_variant);
    base.set_border_width(1);
    base.set_padding(14);
    base.set_radius(18);

    Sytel hover;
    hover.set_border_color(colors.primary);

    Sytel click;
    click.set_border_color(colors.primary);

    SytelRules rules;
    rules.set_base_sytel(base);
    rules.set_on_hover_sytel(hover);
    rules.set_on_click_sytel(click);
    return rules;
}

void populate_setup_inputs()
{
    if (g_host_input != nullptr) {
        g_host_input->set_text(g_config.host);
    }
    if (g_username_input != nullptr) {
        g_username_input->set_text(g_config.username);
    }
    if (g_port_input != nullptr) {
        char port_buffer[16];
        snprintf(port_buffer, sizeof(port_buffer), "%u", static_cast<unsigned int>(g_config.port));
        set_textbox_text(g_port_input, port_buffer);
    }
}

}

static int duckchat_main_impl(int argc, char** argv, char**) {
    const char* theme_to_load = "md3-light";
    if (argc > 1 && argv != nullptr && argv[1] != nullptr && argv[1][0] != '\0') {
        theme_to_load = argv[1];
    }
    stardustui::Theme::load_theme(theme_to_load);
    const stardustui::Colors& colors = stardustui::Theme::colors();

    load_config(g_config);
    g_config_loaded = g_config.host.length() > 0;

    stardustui::string window_title("DuckChat");
    Window window(window_title.c_str(), 1040, 680, true);
    g_window = &window;

    ScreenHost screen_host;
    screen_host.set_bounds(20, 20, 1000, 640);
    screen_host.set_anchors(base_component::AnchorLeft |
                            base_component::AnchorTop |
                            base_component::AnchorRight |
                            base_component::AnchorBottom);
    g_screen_host = &screen_host;

    PollerComponent poller;
    poller.set_update_proc(poll_chat_socket);
    g_poller = &poller;

    FlexLayout setup_screen(1000, 640);
    setup_screen.set_direction(FlexLayout::Column);
    setup_screen.set_gap(16);
    setup_screen.set_padding(28);
    setup_screen.set_style_rules(make_panel_rules(colors.background, colors.background, 0));
    g_setup_screen = &setup_screen;

    Lable setup_title("Connect To Chat Server", 32, colors.on_surface);
    Lable setup_desc("First launch asks for server address and username. It will be stored as JSON.", 14, colors.on_surface_variant);

    FlexLayout setup_card(0, 0);
    setup_card.set_direction(FlexLayout::Column);
    setup_card.set_gap(14);
    setup_card.set_padding(20);
    setup_card.set_style_rules(make_panel_rules(colors.surface, colors.outline_variant));

    Lable host_label("Server IP / Domain", 14, colors.on_surface_variant);
    TextBox host_input(0, 58, true, make_textbox_rules(colors));
    g_host_input = &host_input;

    Lable port_label("Port", 14, colors.on_surface_variant);
    TextBox port_input(0, 58, true, make_textbox_rules(colors));
    g_port_input = &port_input;

    Lable username_label("Username", 14, colors.on_surface_variant);
    TextBox username_input(0, 58, true, make_textbox_rules(colors));
    g_username_input = &username_input;

    Lable setup_hint("Enter chat server information", 13, colors.on_surface_variant);
    g_setup_hint_label = &setup_hint;

    Button connect_button("Save And Connect", 220, 58,
                          make_button_rules(colors,
                                            colors.primary,
                                            colors.on_primary,
                                            colors.secondary,
                                            colors.on_secondary));
    connect_button.callback(on_connect_click);
    g_connect_button = &connect_button;

    setup_card.addComponent(host_label, 0);
    setup_card.addComponent(host_input, 0);
    setup_card.addComponent(port_label, 0);
    setup_card.addComponent(port_input, 0);
    setup_card.addComponent(username_label, 0);
    setup_card.addComponent(username_input, 0);
    setup_card.addComponent(setup_hint, 0);
    setup_card.addComponent(connect_button, 0);

    setup_screen.addComponent(setup_title, 0);
    setup_screen.addComponent(setup_desc, 0);
    setup_screen.addComponent(setup_card, 1);

    FlexLayout chat_screen(1000, 640);
    chat_screen.set_direction(FlexLayout::Row);
    chat_screen.set_gap(12);
    chat_screen.set_padding(0);
    chat_screen.set_style_rules(make_panel_rules(colors.background, colors.background, 0));
    g_chat_screen = &chat_screen;

    FlexLayout sidebar(260, 0);
    sidebar.set_direction(FlexLayout::Column);
    sidebar.set_gap(12);
    sidebar.set_padding(18);
    sidebar.set_style_rules(make_panel_rules(colors.surface_variant, colors.outline_variant));

    Lable sidebar_title("Connection", 24, colors.on_surface);
    Lable sidebar_hint("Stored in JSON config", 14, colors.on_surface_variant);
    Button reconnect_button("Reconnect", 0, 48,
                            make_button_rules(colors,
                                              colors.secondary_container,
                                              colors.on_secondary_container,
                                              colors.secondary,
                                              colors.on_secondary));
    reconnect_button.callback(on_reconnect_click);
    g_reconnect_button = &reconnect_button;

    sidebar.addComponent(sidebar_title, 0);
    sidebar.addComponent(sidebar_hint, 0);
    sidebar.addComponent(reconnect_button, 0);

    FlexLayout main_column(0, 0);
    main_column.set_direction(FlexLayout::Column);
    main_column.set_gap(12);
    main_column.set_padding(0);
    main_column.set_style_rules(make_panel_rules(colors.background, colors.background, 0));

    FlexLayout header_content(0, 84);
    header_content.set_direction(FlexLayout::Row);
    header_content.set_padding(20);
    header_content.set_align_items(FlexLayout::AlignCenter);
    header_content.set_justify_content(FlexLayout::JustifySpaceBetween);
    header_content.set_style_rules(make_panel_rules(colors.surface, colors.outline_variant));

    Lable room_title("Chat Room", 28, colors.on_surface);
    Lable room_meta("", 14, colors.on_surface_variant);
    g_room_meta_label = &room_meta;

    FlexLayout header_labels(0, 44);
    header_labels.set_direction(FlexLayout::Column);
    header_labels.set_gap(4);
    header_labels.addComponent(room_title, 0);
    header_labels.addComponent(room_meta, 0);

    header_content.addComponent(header_labels, 1);

    TextBox history_box(0, 0, false, make_textbox_rules(colors));
    g_history_box = &history_box;

    FlexLayout composer(0, 150);
    composer.set_direction(FlexLayout::Column);
    composer.set_gap(12);
    composer.set_padding(16);
    composer.set_style_rules(make_panel_rules(colors.surface, colors.outline_variant));

    Lable composer_hint("Message", 14, colors.on_surface_variant);

    FlexLayout composer_row(0, 92);
    composer_row.set_direction(FlexLayout::Row);
    composer_row.set_gap(12);
    composer_row.set_align_items(FlexLayout::AlignStretch);
    composer_row.set_style_rules(make_panel_rules(colors.surface, colors.surface, 0));

    TextBox message_input(0, 92, true, make_textbox_rules(colors));
    g_message_input = &message_input;

    Button send_button("Send", 120, 92,
                       make_button_rules(colors,
                                         colors.primary,
                                         colors.on_primary,
                                         colors.secondary,
                                         colors.on_secondary));
    send_button.callback(on_send_click);
    g_send_button = &send_button;

    Lable status_label("", 13, colors.on_surface_variant);
    g_status_label = &status_label;

    composer_row.addComponent(message_input, 1);
    composer_row.addComponent(send_button, 0);
    composer.addComponent(composer_hint, 0);
    composer.addComponent(composer_row, 1);
    composer.addComponent(status_label, 0);

    main_column.addComponent(header_content, 0);
    main_column.addComponent(history_box, 1);
    main_column.addComponent(composer, 0);

    chat_screen.addComponent(sidebar, 0);
    chat_screen.addComponent(main_column, 1);

    window.addComponent(screen_host);
    window.addComponent(poller);

    populate_setup_inputs();
    update_connection_status();

    if (g_config_loaded) {
        append_history_line("[System] Loaded saved configuration.");
        show_screen(&chat_screen);
        connect_chat();
    } else {
        show_screen(&setup_screen);
    }

    window.show();
    return 0;
}

#if defined(STARDUSTUI_WINDOWS) || defined(STARDUSTUI_LINUX)
int main(int argc, char *argv[], char *envp[])
{
    return duckchat_main_impl(argc, argv, envp);
}
#else
extern "C" int duckchat_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int duckchat_main_cpp(int argc, char *argv[], char *envp[])
{
    return duckchat_main_impl(argc, argv, envp);
}
#endif
