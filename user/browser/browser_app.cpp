#include "browser_fetch.h"
#include "browser_image_decode.h"
#include "browser_platform.h"

#include <litehtml.h>
#include <litehtml/url.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "../../include/user/settings.h"

namespace
{

constexpr int BROWSER_WIDTH = 1024;
constexpr int BROWSER_HEIGHT = 720;
constexpr int BROWSER_LOOP_SLEEP_MS = 16;
constexpr int BROWSER_HEADER_HEIGHT = 102;
constexpr int BROWSER_MARGIN = 14;
constexpr int BROWSER_INPUT_X = 252;
constexpr int BROWSER_INPUT_Y = 16;
constexpr int BROWSER_INPUT_HEIGHT = 28;
constexpr int BROWSER_FIND_X = 252;
constexpr int BROWSER_FIND_Y = 52;
constexpr int BROWSER_FIND_HEIGHT = 24;
constexpr int BROWSER_SCROLLBAR_WIDTH = 14;
constexpr int BROWSER_SCROLLBAR_MIN_THUMB = 32;
constexpr int BROWSER_SCROLL_STEP = 48;
constexpr int BROWSER_TEXT_LINE_HEIGHT = 20;
constexpr unsigned char BROWSER_KEY_F3 = 139;
constexpr bool BROWSER_ENABLE_LITEHTML = true;
constexpr const char *BROWSER_USER_CSS =
    "table{font-size:12px;line-height:16px;border-collapse:collapse;}"
    "td,th{font-size:12px;line-height:16px;padding:2px 4px;}";

constexpr std::uint32_t COLOR_BG = 0xe7ecefff;
constexpr std::uint32_t COLOR_PANEL = 0xfafaf9ff;
constexpr std::uint32_t COLOR_HEADER = 0x16324fff;
constexpr std::uint32_t COLOR_INPUT = 0xffffffff;
constexpr std::uint32_t COLOR_TEXT = 0x1f2933ff;
constexpr std::uint32_t COLOR_MUTED = 0xc7d2daff;
constexpr std::uint32_t COLOR_STATUS = 0xb8f2e6ff;
constexpr std::uint32_t COLOR_ERROR = 0xc1121fff;
constexpr std::uint32_t COLOR_LINK = 0x1976d2ff;
constexpr std::uint32_t COLOR_BUTTON = 0x244766ff;
constexpr std::uint32_t COLOR_BUTTON_DISABLED = 0x50667aff;
constexpr std::uint32_t COLOR_HIGHLIGHT = 0xffe08aff;

struct BrowserXFile
{
    unsigned long long length;
    void              *buffer;
};

struct BrowserUserInfo
{
    char name[64];
    int  user_type;
};

extern "C" {
BrowserXFile *xapi_OpenFile(WSTR path);
void          xapi_CloseFile(BrowserXFile *file);
void          xapi_GetCurrentUser(BrowserUserInfo *user_info);
}

static int g_browser_language = XJ380_LANGUAGE_ZH_CN;

static int browser_normalize_language(int language)
{
    return language == XJ380_LANGUAGE_EN_US ? XJ380_LANGUAGE_EN_US : XJ380_LANGUAGE_ZH_CN;
}

static int browser_read_language()
{
    BrowserUserInfo user {};
    xapi_GetCurrentUser(&user);
    if (user.name[0] == '\0') {
        return XJ380_LANGUAGE_ZH_CN;
    }

    char path[256];
    std::snprintf(path, sizeof(path), "/users/%s/settings.dat", user.name);

    BrowserXFile *file = xapi_OpenFile(path);
    if (file == nullptr || file->buffer == nullptr) {
        if (file != nullptr) {
            xapi_CloseFile(file);
        }
        return XJ380_LANGUAGE_ZH_CN;
    }

    int language = XJ380_LANGUAGE_ZH_CN;
    if (file->length >= sizeof(SettingsDataFileFormat)) {
        const SettingsDataFileFormat *settings = static_cast<const SettingsDataFileFormat *>(file->buffer);
        language = settings->Language;
    }

    xapi_CloseFile(file);
    return browser_normalize_language(language);
}

static char *browser_tr(const char *zh_cn, const char *en_us)
{
    return (char *)(browser_normalize_language(g_browser_language) == XJ380_LANGUAGE_EN_US ? en_us : zh_cn);
}

static std::uint32_t litehtml_color(const litehtml::web_color &color)
{
    return xj380_rgba(color.red, color.green, color.blue, color.alpha);
}

static std::string lower_copy(const std::string &value)
{
    std::string lowered = value;
    for (char &ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

static std::string trim_copy(const std::string &value)
{
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

static std::string shorten_url_for_log(const std::string &url)
{
    constexpr std::size_t kMaxLogUrlBytes = 120;
    if (url.size() <= kMaxLogUrlBytes) {
        return url;
    }
    return url.substr(0, kMaxLogUrlBytes) + "...";
}

static void log_line(const std::string &text)
{
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "browser: %s\n", text.c_str());
    xapi_OutputSerial(buffer);
}

static std::size_t find_case_insensitive(const std::string &text, const std::string &needle, std::size_t start = 0)
{
    if (needle.empty() || start >= text.size()) {
        return std::string::npos;
    }

    auto it = std::search(text.begin() + static_cast<std::ptrdiff_t>(start),
                          text.end(),
                          needle.begin(),
                          needle.end(),
                          [](char lhs, char rhs) {
                              return std::tolower(static_cast<unsigned char>(lhs)) ==
                                     std::tolower(static_cast<unsigned char>(rhs));
                          });
    if (it == text.end()) {
        return std::string::npos;
    }
    return static_cast<std::size_t>(it - text.begin());
}

static void erase_tag_block(std::string &html, const char *tag_name)
{
    const std::string open_tag = std::string("<") + tag_name;
    const std::string close_tag = std::string("</") + tag_name;

    std::size_t cursor = 0;
    while (cursor < html.size()) {
        const std::size_t open_pos = find_case_insensitive(html, open_tag, cursor);
        if (open_pos == std::string::npos) {
            break;
        }

        const std::size_t close_pos = find_case_insensitive(html, close_tag, open_pos + open_tag.size());
        if (close_pos == std::string::npos) {
            html.erase(open_pos);
            break;
        }

        const std::size_t close_end = html.find('>', close_pos);
        if (close_end == std::string::npos) {
            html.erase(open_pos);
            break;
        }

        html.erase(open_pos, close_end - open_pos + 1);
        cursor = open_pos;
    }
}

static void erase_tag_markers(std::string &html, const char *tag_name)
{
    const std::string open_tag = std::string("<") + tag_name;
    const std::string close_tag = std::string("</") + tag_name;

    std::size_t cursor = 0;
    while (cursor < html.size()) {
        const std::size_t open_pos = find_case_insensitive(html, open_tag, cursor);
        if (open_pos == std::string::npos) {
            break;
        }
        const std::size_t open_end = html.find('>', open_pos);
        if (open_end == std::string::npos) {
            break;
        }
        html.erase(open_pos, open_end - open_pos + 1);
        cursor = open_pos;
    }

    cursor = 0;
    while (cursor < html.size()) {
        const std::size_t close_pos = find_case_insensitive(html, close_tag, cursor);
        if (close_pos == std::string::npos) {
            break;
        }
        const std::size_t close_end = html.find('>', close_pos);
        if (close_end == std::string::npos) {
            break;
        }
        html.erase(close_pos, close_end - close_pos + 1);
        cursor = close_pos;
    }
}

static std::string escape_html_text(const std::string &text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (unsigned char ch : text) {
        switch (ch) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            if (ch == '\r') {
                continue;
            }
            escaped.push_back(static_cast<char>(ch));
            break;
        }
    }
    return escaped;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool is_identifier_char(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           ch == '_' ||
           ch == '$';
}

static std::size_t skip_js_space(const std::string &text, std::size_t pos)
{
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

static bool decode_js_string_literal(const std::string &text,
                                     std::size_t start,
                                     std::string &value,
                                     std::size_t &next)
{
    value.clear();
    if (start >= text.size()) {
        return false;
    }

    const char quote = text[start];
    if (quote != '"' && quote != '\'' && quote != '`') {
        return false;
    }

    std::size_t pos = start + 1;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == quote) {
            next = pos;
            return true;
        }
        if (ch != '\\') {
            if (quote == '`' && ch == '$' && pos < text.size() && text[pos] == '{') {
                return false;
            }
            value.push_back(ch);
            continue;
        }
        if (pos >= text.size()) {
            return false;
        }

        const char escaped = text[pos++];
        switch (escaped) {
        case '\\':
        case '\'':
        case '"':
        case '`':
            value.push_back(escaped);
            break;
        case 'n':
            value.push_back('\n');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case 't':
            value.push_back('\t');
            break;
        case 'x':
            if (pos + 1 > text.size()) {
                return false;
            } else {
                const int hi = hex_value(text[pos]);
                const int lo = hex_value(text[pos + 1]);
                if (hi < 0 || lo < 0) {
                    return false;
                }
                value.push_back(static_cast<char>((hi << 4) | lo));
                pos += 2;
            }
            break;
        case 'u':
            if (pos + 3 >= text.size()) {
                return false;
            } else {
                int codepoint = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = hex_value(text[pos + i]);
                    if (digit < 0) {
                        return false;
                    }
                    codepoint = (codepoint << 4) | digit;
                }
                pos += 4;
                if (codepoint <= 0x7f) {
                    value.push_back(static_cast<char>(codepoint));
                }
            }
            break;
        default:
            value.push_back(escaped);
            break;
        }
    }

    return false;
}

static bool parse_js_string_expression(const std::string &text,
                                       std::size_t start,
                                       std::string &value,
                                       std::size_t &next)
{
    value.clear();
    std::size_t pos = skip_js_space(text, start);

    std::string part;
    std::size_t part_end = pos;
    if (!decode_js_string_literal(text, pos, part, part_end)) {
        return false;
    }

    value += part;
    pos = skip_js_space(text, part_end);
    while (pos < text.size() && text[pos] == '+') {
        pos = skip_js_space(text, pos + 1);
        if (!decode_js_string_literal(text, pos, part, part_end)) {
            return false;
        }
        value += part;
        pos = skip_js_space(text, part_end);
    }

    next = pos;
    return true;
}

static std::string extract_tag_attribute(const std::string &tag, const std::string &name)
{
    std::size_t pos = 0;
    while (pos < tag.size()) {
        pos = find_case_insensitive(tag, name, pos);
        if (pos == std::string::npos) {
            break;
        }
        if (pos > 0 && is_identifier_char(tag[pos - 1])) {
            pos += name.size();
            continue;
        }

        std::size_t value_pos = skip_js_space(tag, pos + name.size());
        if (value_pos >= tag.size() || tag[value_pos] != '=') {
            pos += name.size();
            continue;
        }

        value_pos = skip_js_space(tag, value_pos + 1);
        if (value_pos >= tag.size()) {
            return std::string();
        }

        if (tag[value_pos] == '"' || tag[value_pos] == '\'') {
            const char quote = tag[value_pos++];
            const std::size_t value_end = tag.find(quote, value_pos);
            if (value_end == std::string::npos) {
                return tag.substr(value_pos);
            }
            return tag.substr(value_pos, value_end - value_pos);
        }

        std::size_t value_end = value_pos;
        while (value_end < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[value_end])) == 0 &&
               tag[value_end] != '>') {
            ++value_end;
        }
        return tag.substr(value_pos, value_end - value_pos);
    }

    return std::string();
}

static bool is_executable_script_type(const std::string &type)
{
    const std::string lowered = lower_copy(trim_copy(type));
    if (lowered.empty()) {
        return true;
    }
    return lowered.find("javascript") != std::string::npos ||
           lowered.find("ecmascript") != std::string::npos ||
           lowered == "module" ||
           lowered == "text/module";
}

static bool find_js_call(const std::string &source,
                         const std::string &needle,
                         std::size_t start,
                         std::size_t &call_pos,
                         std::size_t &arg_pos)
{
    std::size_t pos = start;
    while (true) {
        pos = find_case_insensitive(source, needle, pos);
        if (pos == std::string::npos) {
            return false;
        }
        if (pos > 0 && is_identifier_char(source[pos - 1])) {
            pos += needle.size();
            continue;
        }

        std::size_t open_pos = skip_js_space(source, pos + needle.size());
        if (open_pos < source.size() && source[open_pos] == '(') {
            call_pos = pos;
            arg_pos = open_pos + 1;
            return true;
        }
        pos += needle.size();
    }
}

static bool find_js_assignment(const std::string &source,
                               const std::string &needle,
                               std::size_t start,
                               std::size_t &assign_pos,
                               std::size_t &value_pos)
{
    std::size_t pos = start;
    while (true) {
        pos = find_case_insensitive(source, needle, pos);
        if (pos == std::string::npos) {
            return false;
        }
        if (pos > 0 && is_identifier_char(source[pos - 1])) {
            pos += needle.size();
            continue;
        }

        std::size_t equal_pos = skip_js_space(source, pos + needle.size());
        if (equal_pos < source.size() && source[equal_pos] == '=') {
            assign_pos = pos;
            value_pos = skip_js_space(source, equal_pos + 1);
            return true;
        }
        pos += needle.size();
    }
}

struct BrowserPreparedDocument
{
    bool        fallback_used = false;
    std::string html;
    std::string redirect_url;
    std::string title_override;
    int         script_blocks = 0;
    int         scripts_with_effect = 0;
    int         document_writes = 0;
    int         external_scripts = 0;
};

enum class BrowserInputMode
{
    Url,
    Find,
};

enum class BrowserNavReason
{
    Normal,
    History,
    Reload,
};

enum class BrowserCommand
{
    None,
    Back,
    Forward,
    Reload,
    Stop,
    FocusUrl,
    FocusFind,
    FindPrev,
    FindNext,
};

struct BrowserButton
{
    BrowserCommand command = BrowserCommand::None;
    int            x1 = 0;
    int            y1 = 0;
    int            x2 = 0;
    int            y2 = 0;
    const char    *label = "";
};

static void set_html_title(std::string &html, const std::string &title)
{
    if (title.empty()) {
        return;
    }

    const std::string escaped = escape_html_text(title);
    const std::size_t title_pos = find_case_insensitive(html, "<title");
    if (title_pos != std::string::npos) {
        const std::size_t title_open_end = html.find('>', title_pos);
        const std::size_t title_close_pos = find_case_insensitive(html, "</title", title_open_end == std::string::npos ? title_pos : title_open_end + 1);
        if (title_open_end != std::string::npos && title_close_pos != std::string::npos) {
            html.replace(title_open_end + 1, title_close_pos - title_open_end - 1, escaped);
            return;
        }
    }

    const std::string title_tag = "<title>" + escaped + "</title>";
    const std::size_t head_pos = find_case_insensitive(html, "<head");
    if (head_pos != std::string::npos) {
        const std::size_t head_end = html.find('>', head_pos);
        if (head_end != std::string::npos) {
            html.insert(head_end + 1, title_tag);
            return;
        }
    }

    html.insert(0, "<head>" + title_tag + "</head>");
}

static bool percent_decode(const std::string &input, std::string &output)
{
    output.clear();
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '%') {
            if (i + 2 >= input.size()) {
                return false;
            }
            const int hi = hex_value(input[i + 1]);
            const int lo = hex_value(input[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            output.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            continue;
        }
        output.push_back(ch == '+' ? ' ' : ch);
    }
    return true;
}

static int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

static bool decode_base64(const std::string &input, std::string &output)
{
    output.clear();
    output.reserve((input.size() * 3) / 4);

    int accum = 0;
    int bits = 0;
    for (char ch : input) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const int value = base64_value(ch);
        if (value < 0) {
            return false;
        }

        accum = (accum << 6) | value;
        bits += 6;
        while (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((accum >> bits) & 0xff));
        }
    }
    return true;
}

static bool decode_data_url(const std::string &url, BrowserFetchResult &result)
{
    const std::string lowered = lower_copy(url);
    if (lowered.rfind("data:", 0) != 0) {
        return false;
    }

    const std::size_t comma = url.find(',');
    if (comma == std::string::npos) {
        result.ok = false;
        result.error = browser_tr("data URL 无效", "Invalid data URL");
        return true;
    }

    const std::string meta = url.substr(5, comma - 5);
    const std::string payload = url.substr(comma + 1);
    const std::string lowered_meta = lower_copy(meta);
    const bool is_base64 = lowered_meta.find(";base64") != std::string::npos;

    std::string decoded;
    const bool ok = is_base64 ? decode_base64(payload, decoded) : percent_decode(payload, decoded);
    if (!ok) {
        result.ok = false;
        result.error = is_base64 ? browser_tr("data URL base64 无效", "Invalid data URL base64") :
                                   browser_tr("data URL 转义无效", "Invalid data URL escaping");
        return true;
    }

    std::string content_type = "text/plain;charset=US-ASCII";
    if (!meta.empty()) {
        const std::size_t semi = meta.find(';');
        const std::string explicit_type = trim_copy(meta.substr(0, semi));
        if (!explicit_type.empty()) {
            content_type = explicit_type;
        }
    }

    result.ok = true;
    result.status_code = 200;
    result.final_url = url;
    result.content_type = content_type;
    result.body = std::move(decoded);
    return true;
}

static bool looks_like_html(const BrowserFetchResult &response)
{
    const std::string content_type = lower_copy(response.content_type);
    if (content_type.find("text/html") != std::string::npos ||
        content_type.find("application/xhtml+xml") != std::string::npos) {
        return true;
    }

    const std::size_t probe_len = std::min<std::size_t>(response.body.size(), 1024);
    const std::string probe = response.body.substr(0, probe_len);
    return find_case_insensitive(probe, "<html") != std::string::npos ||
           find_case_insensitive(probe, "<!doctype html") != std::string::npos ||
           find_case_insensitive(probe, "<body") != std::string::npos;
}

static std::string build_fallback_document(const std::string &title,
                                           const std::string &message,
                                           const std::string &excerpt)
{
    std::string html;
    html.reserve(excerpt.size() + 512);
    html += "<html><head><meta charset=\"utf-8\"><title>";
    html += escape_html_text(title);
    html += "</title></head><body style=\"font-family:sans-serif;background:#fafaf9;color:#1f2933;padding:24px;\">";
    html += "<h1 style=\"font-size:24px;margin:0 0 12px 0;\">";
    html += escape_html_text(title);
    html += "</h1><p style=\"margin:0 0 16px 0;\">";
    html += escape_html_text(message);
    html += "</p><pre style=\"white-space:pre-wrap;word-break:break-word;background:#ffffff;border:1px solid #c7d2da;padding:12px;\">";
    html += escape_html_text(excerpt);
    html += "</pre></body></html>";
    return html;
}

static std::string decode_basic_entities(const std::string &text)
{
    std::string decoded;
    decoded.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            decoded.push_back(text[i]);
            continue;
        }

        if (text.compare(i, 5, "&amp;") == 0) {
            decoded.push_back('&');
            i += 4;
        } else if (text.compare(i, 4, "&lt;") == 0) {
            decoded.push_back('<');
            i += 3;
        } else if (text.compare(i, 4, "&gt;") == 0) {
            decoded.push_back('>');
            i += 3;
        } else if (text.compare(i, 6, "&quot;") == 0) {
            decoded.push_back('"');
            i += 5;
        } else if (text.compare(i, 5, "&#39;") == 0) {
            decoded.push_back('\'');
            i += 4;
        } else if (text.compare(i, 6, "&nbsp;") == 0) {
            decoded.push_back(' ');
            i += 5;
        } else {
            decoded.push_back('&');
        }
    }

    return decoded;
}

static std::string html_to_text(std::string html)
{
    erase_tag_block(html, "script");
    erase_tag_block(html, "style");
    erase_tag_markers(html, "noscript");

    std::string text;
    text.reserve(html.size());

    bool in_tag = false;
    bool last_was_space = false;
    for (std::size_t i = 0; i < html.size(); ++i) {
        const char ch = html[i];

        if (!in_tag && ch == '<') {
            const std::size_t tag_end = html.find('>', i);
            const std::size_t tag_text_start = i + 1;
            const std::size_t tag_text_len = (tag_end == std::string::npos ? html.size() : tag_end) - tag_text_start;
            const std::string tag = lower_copy(trim_copy(html.substr(tag_text_start, tag_text_len)));
            const bool closing_tag = !tag.empty() && tag.front() == '/';
            const std::size_t tag_name_start = closing_tag ? 1 : 0;
            std::size_t tag_name_end = tag_name_start;
            while (tag_name_end < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[tag_name_end])) == 0 &&
                   tag[tag_name_end] != '/' &&
                   tag[tag_name_end] != '>') {
                ++tag_name_end;
            }
            const std::string tag_name = tag.substr(tag_name_start, tag_name_end - tag_name_start);

            if (tag_name == "br" || tag_name == "p" || tag_name == "div" || tag_name == "li" ||
                tag_name == "h1" || tag_name == "h2" || tag_name == "h3" || tag_name == "hr" ||
                tag_name == "table" || tag_name == "tr") {
                if (text.empty() || text.back() != '\n') {
                    text.push_back('\n');
                }
                last_was_space = false;
            }
            if (!closing_tag && (tag_name == "td" || tag_name == "th")) {
                if (!text.empty() && text.back() != '\n' && text.back() != '\t') {
                    text.push_back('\t');
                }
                last_was_space = false;
            }

            in_tag = true;
            continue;
        }

        if (in_tag) {
            if (ch == '>') {
                in_tag = false;
            }
            continue;
        }

        if (ch == '\r') {
            continue;
        }
        if (ch == '\n' || ch == '\t') {
            if (!text.empty() && text.back() != '\n') {
                text.push_back('\n');
            }
            last_was_space = false;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!last_was_space && !text.empty() && text.back() != '\n') {
                text.push_back(' ');
                last_was_space = true;
            }
            continue;
        }

        text.push_back(ch);
        last_was_space = false;
    }

    return decode_basic_entities(text);
}

static std::vector<std::string> wrap_text_for_width(const std::string &text, int pixel_width)
{
    std::vector<std::string> lines;
    std::string current_line;

    const auto flush_line = [&]() {
        lines.push_back(current_line);
        current_line.clear();
    };

    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        std::size_t line_end = text.find('\n', cursor);
        const bool explicit_break = line_end != std::string::npos;
        const std::string raw_line = explicit_break ? text.substr(cursor, line_end - cursor) : text.substr(cursor);

        if (raw_line.empty()) {
            lines.emplace_back();
        } else {
            std::size_t start = 0;
            while (start < raw_line.size()) {
                while (start < raw_line.size() && raw_line[start] == ' ') {
                    ++start;
                }
                if (start >= raw_line.size()) {
                    break;
                }

                std::size_t end = start;
                std::string candidate;
                std::size_t last_fit = start;

                while (end <= raw_line.size()) {
                    const std::string part = raw_line.substr(start, end - start);
                    const unsigned long long width = xapi_CalcTextWidth(const_cast<char *>(part.c_str()), 16);
                    if (width > static_cast<unsigned long long>(pixel_width) && !candidate.empty()) {
                        break;
                    }
                    if (width <= static_cast<unsigned long long>(pixel_width)) {
                        candidate = part;
                        last_fit = end;
                    }

                    std::size_t next_space = raw_line.find(' ', end);
                    if (next_space == std::string::npos) {
                        end = raw_line.size();
                    } else if (next_space == end) {
                        ++end;
                    } else {
                        end = next_space;
                    }

                    if (end >= raw_line.size()) {
                        const std::string tail = raw_line.substr(start);
                        const unsigned long long tail_width = xapi_CalcTextWidth(const_cast<char *>(tail.c_str()), 16);
                        if (tail_width <= static_cast<unsigned long long>(pixel_width)) {
                            candidate = tail;
                            last_fit = raw_line.size();
                        }
                        break;
                    }
                }

                if (candidate.empty()) {
                    std::size_t hard_end = start + 1;
                    while (hard_end <= raw_line.size()) {
                        const std::string part = raw_line.substr(start, hard_end - start);
                        const unsigned long long width = xapi_CalcTextWidth(const_cast<char *>(part.c_str()), 16);
                        if (width > static_cast<unsigned long long>(pixel_width) && hard_end > start + 1) {
                            --hard_end;
                            break;
                        }
                        if (hard_end == raw_line.size()) {
                            break;
                        }
                        ++hard_end;
                    }
                    candidate = raw_line.substr(start, hard_end - start);
                    last_fit = hard_end;
                }

                current_line = trim_copy(candidate);
                flush_line();
                start = last_fit;
            }
        }

        if (!explicit_break) {
            break;
        }
        cursor = line_end + 1;
    }

    if (lines.empty()) {
        lines.emplace_back(browser_tr("没有内容。", "No content."));
    }

    return lines;
}

class BrowserApp;

class BrowserContainer final : public litehtml::document_container
{
public:
    explicit BrowserContainer(BrowserApp &app) : app_(app) {}

    litehtml::uint_ptr create_font(const litehtml::font_description &descr,
                                   const litehtml::document *,
                                   litehtml::font_metrics *fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc,
                   const char *text,
                   litehtml::uint_ptr hFont,
                   litehtml::web_color color,
                   const litehtml::position &pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override { return pt * 96.0f / 72.0f; }
    litehtml::pixel_t get_default_font_size() const override { return 16.0f; }
    const char *get_default_font_name() const override { return "sans"; }
    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker &marker) override;
    void load_image(const char *src, const char *baseurl, bool redraw_on_ready) override;
    void get_image_size(const char *src, const char *baseurl, litehtml::size &sz) override;
    void draw_image(litehtml::uint_ptr hdc,
                    const litehtml::background_layer &layer,
                    const std::string &url,
                    const std::string &base_url) override;
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const litehtml::web_color &color) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc,
                              const litehtml::background_layer &layer,
                              const litehtml::background_layer::linear_gradient &gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc,
                              const litehtml::background_layer &layer,
                              const litehtml::background_layer::radial_gradient &gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc,
                             const litehtml::background_layer &layer,
                             const litehtml::background_layer::conic_gradient &gradient) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders &borders, const litehtml::position &draw_pos, bool root) override;

    void set_caption(const char *caption) override;
    void set_base_url(const char *base_url) override;
    void link(const std::shared_ptr<litehtml::document> &, const litehtml::element::ptr &) override {}
    void on_anchor_click(const char *url, const litehtml::element::ptr &) override;
    void on_mouse_event(const litehtml::element::ptr &, litehtml::mouse_event) override {}
    void set_cursor(const char *) override {}
    void transform_text(litehtml::string &text, litehtml::text_transform tt) override;
    void import_css(litehtml::string &text, const litehtml::string &url, litehtml::string &baseurl) override;
    void set_clip(const litehtml::position &pos, const litehtml::border_radiuses &) override;
    void del_clip() override;
    void get_viewport(litehtml::position &viewport) const override { viewport = viewport_; }
    litehtml::element::ptr create_element(const char *, const litehtml::string_map &, const std::shared_ptr<litehtml::document> &) override
    {
        return nullptr;
    }

    void get_media_features(litehtml::media_features &media) const override;
    void get_language(litehtml::string &language, litehtml::string &culture) const override;

    void set_viewport(int width, int height)
    {
        viewport_.x = 0;
        viewport_.y = 0;
        viewport_.width = static_cast<float>(width);
        viewport_.height = static_cast<float>(height);
    }

private:
    struct FontHandle
    {
        int                    size = 16;
        litehtml::font_metrics metrics {};
    };

    BrowserApp                &app_;
    litehtml::position         viewport_;
    std::vector<litehtml::position> clip_stack_;

    litehtml::position current_clip() const;
    void fill_rect(HDLE hdc, const litehtml::position &pos, std::uint32_t color) const;
    void stroke_border(HDLE hdc,
                       float x1,
                       float y1,
                       float x2,
                       float y2,
                       float width,
                       std::uint32_t color) const;
    void blit_image(HDLE hdc,
                    const BrowserDecodedImage &image,
                    const litehtml::position &dest,
                    const litehtml::position &clip) const;
};

class BrowserApp
{
public:
    BrowserApp() : container_(*this)
    {
        g_browser_language = browser_read_language();
        std::snprintf(url_input_, sizeof(url_input_), "%s", "http://neverssl.com/");
        input_len_ = static_cast<int>(std::strlen(url_input_));
        input_cursor_ = input_len_;
        std::snprintf(status_, sizeof(status_), "%s", browser_tr("就绪", "Ready"));
    }

    int run();
    void handle_message(unsigned long long type, unsigned long long hData, unsigned long long lData);
    void request_navigation(const std::string &url);
    void set_title(const std::string &title);
    void set_base_url(const std::string &url) { base_url_ = url; }
    const std::string &base_url() const { return base_url_; }
    std::string resolve_url(const std::string &url, const std::string &base_url) const;
    BrowserFetchResult fetch_resource(const std::string &url);
    const BrowserDecodedImage *load_image_resource(const std::string &url);
    void invalidate_view() { invalidate_content(); }

private:
    struct CachedResource
    {
        std::string        url;
        BrowserFetchResult result;
    };

    struct CachedImage
    {
        std::string         url;
        BrowserDecodedImage image;
        bool                failed = false;
    };

    HDLE                   window_ = 0;
    BrowserContainer       container_;
    litehtml::document::ptr document_;
    std::string            base_url_;
    std::string            pending_url_;
    std::string            page_title_;
    bool                   need_redraw_ = true;
    bool                   frame_dirty_ = true;
    bool                   header_dirty_ = true;
    bool                   content_dirty_ = true;
    bool                   load_pending_ = false;
    bool                   startup_guard_ = true;
    bool                   suppress_history_record_ = false;
    BrowserNavReason       pending_nav_reason_ = BrowserNavReason::Normal;
    BrowserInputMode       input_mode_ = BrowserInputMode::Url;
    int                    window_width_ = BROWSER_WIDTH;
    int                    window_height_ = BROWSER_HEIGHT;
    int                    scroll_y_ = 0;
    int                    text_content_height_ = 0;
    unsigned int           navigation_serial_ = 0;
    char                   url_input_[512] {};
    int                    input_len_ = 0;
    int                    input_cursor_ = 0;
    char                   find_input_[128] {};
    int                    find_len_ = 0;
    int                    find_cursor_ = 0;
    int                    find_match_index_ = -1;
    int                    find_match_count_ = 0;
    char                   status_[128] {};
    std::string            hovered_url_;
    std::string            page_text_;
    std::string            current_url_;
    std::vector<std::string> history_;
    int                    history_index_ = -1;
    std::vector<std::string> text_lines_;
    std::vector<CachedResource> resource_cache_;
    std::vector<CachedImage> image_cache_;

    void invalidate_header() { header_dirty_ = true; need_redraw_ = true; }
    void invalidate_content() { content_dirty_ = true; need_redraw_ = true; }
    void invalidate_all() { frame_dirty_ = true; header_dirty_ = true; content_dirty_ = true; need_redraw_ = true; }
    void load_current_url();
    void render();
    void render_frame();
    void render_header();
    void render_content();
    void refresh_content_view();
    void set_status(const std::string &text);
    void prepare_text_view(const BrowserFetchResult &response);
    BrowserPreparedDocument prepare_document_html(const BrowserFetchResult &response);
    std::string run_script_compat(const std::string &script_source,
                                  const std::string &script_base_url,
                                  BrowserPreparedDocument &prepared);
    const BrowserFetchResult *find_cached_resource(const std::string &url) const;
    void store_cached_resource(const std::string &url, const BrowserFetchResult &result);
    const CachedImage *find_cached_image(const std::string &url) const;
    int  input_right() const { return window_width_ - BROWSER_MARGIN - 152; }
    int  find_right() const { return window_width_ - BROWSER_MARGIN - 152; }
    int  content_x() const { return BROWSER_MARGIN; }
    int  content_y() const { return BROWSER_HEADER_HEIGHT + BROWSER_MARGIN; }
    int  content_width() const { return window_width_ - BROWSER_MARGIN * 2 - BROWSER_SCROLLBAR_WIDTH; }
    int  content_height() const { return window_height_ - content_y() - BROWSER_MARGIN; }
    int  scrollbar_x() const { return content_x() + content_width(); }
    int  scroll_content_height() const;
    void clamp_scroll();
    void draw_scrollbar();
    bool handle_scrollbar_click(int x, int y);
    bool hit_content_area(int x, int y) const;
    bool hit_url_input(int x, int y) const;
    bool hit_find_input(int x, int y) const;
    BrowserCommand command_at(int x, int y) const;
    bool command_enabled(BrowserCommand command) const;
    void execute_command(BrowserCommand command);
    void navigate_to(const std::string &url, BrowserNavReason reason = BrowserNavReason::Normal);
    void record_history(const std::string &url);
    void go_history(int delta);
    void reload_current();
    void stop_loading();
    void focus_url(bool select_all);
    void focus_find(bool select_all);
    void insert_char(char ch);
    void backspace_input();
    void update_find_matches();
    void find_next(int direction);
    std::string clipped_tail_for_width(const std::string &text, int width) const;
    int  text_width_px(const std::string &text, unsigned int size) const;
    void draw_text_clipped(int x, int y, int width, const std::string &text, std::uint32_t color);
    void draw_input_text(int x, int y, int width, const std::string &text, std::uint32_t color);
    void draw_button(const BrowserButton &button);
    std::string href_from_element(litehtml::element::const_ptr element) const;
    void process_pointer_move(int x, int y);
    void process_pointer_click(int x, int y);
};

static BrowserApp *g_app = nullptr;

litehtml::position BrowserContainer::current_clip() const
{
    if (clip_stack_.empty()) {
        return viewport_;
    }
    return clip_stack_.back();
}

void BrowserContainer::fill_rect(HDLE hdc, const litehtml::position &pos, std::uint32_t color) const
{
    litehtml::position clipped = pos.intersect(current_clip());
    if (clipped.width <= 0 || clipped.height <= 0) {
        return;
    }

    xapi_DrawRect(hdc,
                  static_cast<std::uint32_t>(clipped.x),
                  static_cast<std::uint32_t>(clipped.y),
                  static_cast<std::uint32_t>(clipped.right()),
                  static_cast<std::uint32_t>(clipped.bottom()),
                  color,
                  true);
}

void BrowserContainer::stroke_border(HDLE hdc,
                                     float x1,
                                     float y1,
                                     float x2,
                                     float y2,
                                     float width,
                                     std::uint32_t color) const
{
    if (width <= 0.0f || x2 <= x1 || y2 <= y1) {
        return;
    }

    litehtml::position rect(x1, y1, x2 - x1, y2 - y1);
    fill_rect(hdc, rect, color);
}

litehtml::uint_ptr BrowserContainer::create_font(const litehtml::font_description &descr,
                                                 const litehtml::document *,
                                                 litehtml::font_metrics *fm)
{
    FontHandle *font = new FontHandle();
    font->size = std::max(10, static_cast<int>(descr.size));
    font->metrics.font_size = static_cast<float>(font->size);
    font->metrics.height = static_cast<float>(font->size + 4);
    font->metrics.ascent = static_cast<float>(font->size);
    font->metrics.descent = 4.0f;
    font->metrics.x_height = static_cast<float>(font->size * 2 / 3);
    font->metrics.ch_width = static_cast<float>(xapi_CalcTextWidth(const_cast<char *>("0"), static_cast<std::uint32_t>(font->size)));
    font->metrics.draw_spaces = true;
    font->metrics.sub_shift = 2.0f;
    font->metrics.super_shift = 2.0f;

    if (fm != nullptr) {
        *fm = font->metrics;
    }
    return reinterpret_cast<litehtml::uint_ptr>(font);
}

void BrowserContainer::delete_font(litehtml::uint_ptr hFont)
{
    delete reinterpret_cast<FontHandle *>(hFont);
}

litehtml::pixel_t BrowserContainer::text_width(const char *text, litehtml::uint_ptr hFont)
{
    FontHandle *font = reinterpret_cast<FontHandle *>(hFont);
    if (text == nullptr || font == nullptr) {
        return 0;
    }
    return static_cast<float>(xapi_CalcTextWidth(const_cast<char *>(text), static_cast<std::uint32_t>(font->size)));
}

void BrowserContainer::draw_text(litehtml::uint_ptr hdc,
                                 const char *text,
                                 litehtml::uint_ptr hFont,
                                 litehtml::web_color color,
                                 const litehtml::position &pos)
{
    FontHandle *font = reinterpret_cast<FontHandle *>(hFont);
    if (text == nullptr || font == nullptr) {
        return;
    }

    litehtml::position clip = current_clip();
    if (!clip.does_intersect(&pos)) {
        return;
    }

    xapi_DrawText(static_cast<HDLE>(hdc),
                  static_cast<std::uint32_t>(pos.x),
                  static_cast<std::uint32_t>(pos.y),
                  const_cast<char *>(text),
                  static_cast<std::uint32_t>(font->size),
                  litehtml_color(color));
}

void BrowserContainer::draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker &marker)
{
    litehtml::position pos(marker.pos.x, marker.pos.y, 10.0f, 10.0f);
    fill_rect(static_cast<HDLE>(hdc), pos, litehtml_color(marker.color));
}

void BrowserContainer::load_image(const char *src, const char *baseurl, bool redraw_on_ready)
{
    const std::string resolved = app_.resolve_url(src == nullptr ? std::string() : std::string(src),
                                                  baseurl == nullptr ? std::string() : std::string(baseurl));
    if (resolved.empty()) {
        return;
    }

    if (app_.load_image_resource(resolved) != nullptr && redraw_on_ready) {
        app_.invalidate_view();
    }
}

void BrowserContainer::get_image_size(const char *src, const char *baseurl, litehtml::size &sz)
{
    sz.width = 0;
    sz.height = 0;

    const std::string resolved = app_.resolve_url(src == nullptr ? std::string() : std::string(src),
                                                  baseurl == nullptr ? std::string() : std::string(baseurl));
    if (resolved.empty()) {
        return;
    }

    const BrowserDecodedImage *image = app_.load_image_resource(resolved);
    if (image == nullptr || image->empty()) {
        return;
    }

    sz.width = static_cast<float>(image->width);
    sz.height = static_cast<float>(image->height);
}

void BrowserContainer::draw_image(litehtml::uint_ptr hdc,
                                  const litehtml::background_layer &layer,
                                  const std::string &url,
                                  const std::string &base_url)
{
    const std::string resolved = app_.resolve_url(url, base_url);
    if (resolved.empty()) {
        return;
    }

    const BrowserDecodedImage *image = app_.load_image_resource(resolved);
    if (image == nullptr || image->empty()) {
        return;
    }

    litehtml::position dest = layer.origin_box;
    if (dest.width <= 0 || dest.height <= 0) {
        dest = layer.border_box;
    }

    litehtml::position clip = layer.clip_box.intersect(current_clip());
    blit_image(static_cast<HDLE>(hdc), *image, dest, clip);
}

void BrowserContainer::blit_image(HDLE hdc,
                                  const BrowserDecodedImage &image,
                                  const litehtml::position &dest,
                                  const litehtml::position &clip) const
{
    if (image.empty() || dest.width <= 0 || dest.height <= 0 || clip.width <= 0 || clip.height <= 0) {
        return;
    }

    const int left = std::max(static_cast<int>(std::floor(dest.x)), static_cast<int>(std::floor(clip.x)));
    const int top = std::max(static_cast<int>(std::floor(dest.y)), static_cast<int>(std::floor(clip.y)));
    const int right = std::min(static_cast<int>(std::ceil(dest.right())), static_cast<int>(std::ceil(clip.right())));
    const int bottom = std::min(static_cast<int>(std::ceil(dest.bottom())), static_cast<int>(std::ceil(clip.bottom())));
    if (right <= left || bottom <= top) {
        return;
    }

    const std::size_t draw_width = static_cast<std::size_t>(right - left);
    const std::size_t draw_height = static_cast<std::size_t>(bottom - top);
    std::vector<XCOLORA> buffer(draw_width * draw_height);

    for (std::size_t y = 0; y < draw_height; ++y) {
        const float sample_y = ((static_cast<float>(top) + static_cast<float>(y) + 0.5f) - dest.y) / dest.height;
        int src_y = static_cast<int>(sample_y * static_cast<float>(image.height));
        src_y = std::clamp(src_y, 0, image.height - 1);

        for (std::size_t x = 0; x < draw_width; ++x) {
            const float sample_x = ((static_cast<float>(left) + static_cast<float>(x) + 0.5f) - dest.x) / dest.width;
            int src_x = static_cast<int>(sample_x * static_cast<float>(image.width));
            src_x = std::clamp(src_x, 0, image.width - 1);
            buffer[y * draw_width + x] = image.pixels[static_cast<std::size_t>(src_y) * static_cast<std::size_t>(image.width) +
                                                     static_cast<std::size_t>(src_x)];
        }
    }

    xapi_WriteBufferA(hdc,
                      static_cast<unsigned int>(left),
                      static_cast<unsigned int>(top),
                      static_cast<unsigned int>(draw_width),
                      static_cast<unsigned int>(draw_height),
                      buffer.data());
}

void BrowserContainer::draw_solid_fill(litehtml::uint_ptr hdc,
                                       const litehtml::background_layer &layer,
                                       const litehtml::web_color &color)
{
    fill_rect(static_cast<HDLE>(hdc), layer.border_box, litehtml_color(color));
}

void BrowserContainer::draw_linear_gradient(litehtml::uint_ptr hdc,
                                            const litehtml::background_layer &layer,
                                            const litehtml::background_layer::linear_gradient &gradient)
{
    if (!gradient.color_points.empty()) {
        draw_solid_fill(hdc, layer, gradient.color_points.front().color);
    }
}

void BrowserContainer::draw_radial_gradient(litehtml::uint_ptr hdc,
                                            const litehtml::background_layer &layer,
                                            const litehtml::background_layer::radial_gradient &gradient)
{
    if (!gradient.color_points.empty()) {
        draw_solid_fill(hdc, layer, gradient.color_points.front().color);
    }
}

void BrowserContainer::draw_conic_gradient(litehtml::uint_ptr hdc,
                                           const litehtml::background_layer &layer,
                                           const litehtml::background_layer::conic_gradient &gradient)
{
    if (!gradient.color_points.empty()) {
        draw_solid_fill(hdc, layer, gradient.color_points.front().color);
    }
}

void BrowserContainer::draw_borders(litehtml::uint_ptr hdc,
                                    const litehtml::borders &borders,
                                    const litehtml::position &draw_pos,
                                    bool)
{
    const HDLE handle = static_cast<HDLE>(hdc);
    stroke_border(handle, draw_pos.x, draw_pos.y, draw_pos.right(), draw_pos.y + borders.top.width, borders.top.width, litehtml_color(borders.top.color));
    stroke_border(handle, draw_pos.x, draw_pos.bottom() - borders.bottom.width, draw_pos.right(), draw_pos.bottom(), borders.bottom.width, litehtml_color(borders.bottom.color));
    stroke_border(handle, draw_pos.x, draw_pos.y, draw_pos.x + borders.left.width, draw_pos.bottom(), borders.left.width, litehtml_color(borders.left.color));
    stroke_border(handle, draw_pos.right() - borders.right.width, draw_pos.y, draw_pos.right(), draw_pos.bottom(), borders.right.width, litehtml_color(borders.right.color));
}

void BrowserContainer::set_caption(const char *caption)
{
    app_.set_title(caption == nullptr ? std::string() : std::string(caption));
}

void BrowserContainer::set_base_url(const char *base_url)
{
    app_.set_base_url(base_url == nullptr ? std::string() : std::string(base_url));
}

void BrowserContainer::on_anchor_click(const char *url, const litehtml::element::ptr &)
{
    if (url == nullptr || *url == '\0') {
        return;
    }

    app_.request_navigation(app_.resolve_url(url, app_.base_url()));
}

void BrowserContainer::import_css(litehtml::string &text, const litehtml::string &url, litehtml::string &baseurl)
{
    text.clear();

    if (url.empty()) {
        return;
    }

    std::string resolve_base = baseurl.empty() ? app_.base_url() : baseurl;
    if (resolve_base.empty()) {
        resolve_base = app_.base_url();
    }

    litehtml::url resolved = resolve_base.empty() ? litehtml::url(url)
                                                  : litehtml::resolve(litehtml::url(resolve_base), litehtml::url(url));

    BrowserFetchResult response = app_.fetch_resource(resolved.str());
    if (!response.ok) {
        log_line("CSS 加载失败：" + resolved.str() + " 错误=" + response.error);
        return;
    }

    text = response.body;
    baseurl = response.final_url.empty() ? resolved.str() : response.final_url;
}

void BrowserContainer::transform_text(litehtml::string &text, litehtml::text_transform tt)
{
    switch (tt) {
    case litehtml::text_transform_uppercase:
        for (char &ch : text) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        break;
    case litehtml::text_transform_lowercase:
        for (char &ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        break;
    case litehtml::text_transform_capitalize: {
        bool capitalize = true;
        for (char &ch : text) {
            if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
                capitalize = true;
            } else if (capitalize) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                capitalize = false;
            }
        }
        break;
    }
    default:
        break;
    }
}

void BrowserContainer::set_clip(const litehtml::position &pos, const litehtml::border_radiuses &)
{
    clip_stack_.push_back(pos.intersect(current_clip()));
}

void BrowserContainer::del_clip()
{
    if (!clip_stack_.empty()) {
        clip_stack_.pop_back();
    }
}

void BrowserContainer::get_media_features(litehtml::media_features &media) const
{
    media.type = litehtml::media_type_screen;
    media.width = viewport_.width;
    media.height = viewport_.height;
    media.device_width = viewport_.width;
    media.device_height = viewport_.height;
    media.color = 8;
    media.color_index = 256;
    media.monochrome = 0;
    media.resolution = 96.0f;
}

void BrowserContainer::get_language(litehtml::string &language, litehtml::string &culture) const
{
    if (g_browser_language == XJ380_LANGUAGE_EN_US) {
        language = "en";
        culture = "US";
    } else {
        language = "zh";
        culture = "CN";
    }
}

void BrowserApp::set_title(const std::string &title)
{
    page_title_ = title;
    invalidate_header();
}

void BrowserApp::set_status(const std::string &text)
{
    std::snprintf(status_, sizeof(status_), "%s", text.c_str());
    invalidate_header();
}

std::string BrowserApp::run_script_compat(const std::string &script_source,
                                          const std::string &script_base_url,
                                          BrowserPreparedDocument &prepared)
{
    std::string injected_html;

    std::size_t search_pos = 0;
    while (search_pos < script_source.size()) {
        std::size_t write_call = std::string::npos;
        std::size_t write_arg = 0;
        std::size_t writeln_call = std::string::npos;
        std::size_t writeln_arg = 0;
        const bool has_write = find_js_call(script_source, "document.write", search_pos, write_call, write_arg);
        const bool has_writeln = find_js_call(script_source, "document.writeln", search_pos, writeln_call, writeln_arg);
        if (!has_write && !has_writeln) {
            break;
        }

        const bool use_writeln = !has_write || (has_writeln && writeln_call < write_call);
        const std::size_t call_pos = use_writeln ? writeln_call : write_call;
        const std::size_t arg_pos = use_writeln ? writeln_arg : write_arg;

        std::string value;
        std::size_t after_value = 0;
        if (parse_js_string_expression(script_source, arg_pos, value, after_value)) {
            after_value = skip_js_space(script_source, after_value);
            if (after_value < script_source.size() && script_source[after_value] == ')') {
                injected_html += value;
                if (use_writeln) {
                    injected_html.push_back('\n');
                }
                ++prepared.document_writes;
            }
        }

        search_pos = call_pos + 1;
    }

    if (prepared.redirect_url.empty()) {
        std::string redirect_value;
        std::size_t match_pos = std::string::npos;
        std::size_t value_pos = 0;
        std::size_t after_value = 0;

        const auto try_redirect_call = [&](const char *needle) -> bool {
            if (!find_js_call(script_source, needle, 0, match_pos, value_pos)) {
                return false;
            }
            if (!parse_js_string_expression(script_source, value_pos, redirect_value, after_value)) {
                return false;
            }
            after_value = skip_js_space(script_source, after_value);
            return after_value < script_source.size() && script_source[after_value] == ')';
        };

        const auto try_redirect_assignment = [&](const char *needle) -> bool {
            if (!find_js_assignment(script_source, needle, 0, match_pos, value_pos)) {
                return false;
            }
            return parse_js_string_expression(script_source, value_pos, redirect_value, after_value);
        };

        if (try_redirect_call("window.location.replace") ||
            try_redirect_call("location.replace") ||
            try_redirect_call("window.location.assign") ||
            try_redirect_call("location.assign") ||
            try_redirect_assignment("window.location.href") ||
            try_redirect_assignment("location.href") ||
            try_redirect_assignment("window.location")) {
            prepared.redirect_url = resolve_url(redirect_value, script_base_url);
        }
    }

    if (prepared.title_override.empty()) {
        std::string title_value;
        std::size_t match_pos = std::string::npos;
        std::size_t value_pos = 0;
        std::size_t after_value = 0;
        if (find_js_assignment(script_source, "document.title", 0, match_pos, value_pos) &&
            parse_js_string_expression(script_source, value_pos, title_value, after_value)) {
            prepared.title_override = title_value;
        }
    }

    return injected_html;
}

BrowserPreparedDocument BrowserApp::prepare_document_html(const BrowserFetchResult &response)
{
    BrowserPreparedDocument prepared;

    if (!looks_like_html(response)) {
        prepared.fallback_used = true;
        return prepared;
    }

    std::string html = response.body;
    erase_tag_markers(html, "noscript");

    std::string output;
    output.reserve(html.size());

    std::size_t cursor = 0;
    while (cursor < html.size()) {
        const std::size_t script_pos = find_case_insensitive(html, "<script", cursor);
        if (script_pos == std::string::npos) {
            output.append(html, cursor, html.size() - cursor);
            break;
        }

        output.append(html, cursor, script_pos - cursor);

        const std::size_t open_end = html.find('>', script_pos);
        if (open_end == std::string::npos) {
            break;
        }
        const std::size_t close_pos = find_case_insensitive(html, "</script", open_end + 1);
        if (close_pos == std::string::npos) {
            break;
        }
        const std::size_t close_end = html.find('>', close_pos);
        if (close_end == std::string::npos) {
            break;
        }

        ++prepared.script_blocks;

        const std::string open_tag = html.substr(script_pos, open_end - script_pos + 1);
        const std::string type = extract_tag_attribute(open_tag, "type");
        if (is_executable_script_type(type)) {
            std::string script_base_url = base_url_;
            std::string script_source = html.substr(open_end + 1, close_pos - open_end - 1);

            const std::string src = extract_tag_attribute(open_tag, "src");
            if (!src.empty()) {
                const std::string resolved = resolve_url(src, base_url_);
                if (!resolved.empty()) {
                    BrowserFetchResult script_response = fetch_resource(resolved);
                    if (script_response.ok) {
                        script_source = script_response.body;
                        script_base_url = script_response.final_url.empty() ? resolved : script_response.final_url;
                        ++prepared.external_scripts;
                    } else {
                        log_line("脚本加载失败：" + shorten_url_for_log(resolved) + " 错误=" + script_response.error);
                        script_source.clear();
                    }
                }
            }

            const int writes_before = prepared.document_writes;
            const bool had_redirect = !prepared.redirect_url.empty();
            const bool had_title = !prepared.title_override.empty();
            std::string injected = run_script_compat(script_source, script_base_url, prepared);
            if (!injected.empty() ||
                prepared.document_writes != writes_before ||
                (!had_redirect && !prepared.redirect_url.empty()) ||
                (!had_title && !prepared.title_override.empty())) {
                ++prepared.scripts_with_effect;
            }
            output += injected;
        }

        cursor = close_end + 1;
    }

    html.swap(output);

    constexpr std::size_t kMaxHtmlBytes = 256 * 1024;
    if (html.size() > kMaxHtmlBytes) {
        prepared.fallback_used = true;
        return prepared;
    }

    if (!prepared.title_override.empty()) {
        set_html_title(html, prepared.title_override);
    }

    if (find_case_insensitive(html, "<html") == std::string::npos &&
        find_case_insensitive(html, "<body") == std::string::npos) {
        html = build_fallback_document(browser_tr("HTML 片段", "HTML Fragment"),
                                       browser_tr("响应不是完整 HTML 文档，已使用简化包装进行渲染。",
                                                  "The response is not a full HTML document, so it was rendered in a simplified wrapper."),
                                       html);
    }

    prepared.html = std::move(html);
    return prepared;
}

std::string BrowserApp::resolve_url(const std::string &url, const std::string &base_url) const
{
    const std::string trimmed = trim_copy(url);
    if (trimmed.empty()) {
        return std::string();
    }
    if (lower_copy(trimmed).rfind("data:", 0) == 0) {
        return trimmed;
    }

    std::string resolve_base = trim_copy(base_url);
    if (resolve_base.empty()) {
        resolve_base = base_url_.empty() ? std::string() : base_url_;
    }

    litehtml::url resolved = resolve_base.empty() ? litehtml::url(trimmed)
                                                  : litehtml::resolve(litehtml::url(resolve_base), litehtml::url(trimmed));
    const std::string final_url = resolved.str();
    const std::string scheme = lower_copy(final_url.substr(0, final_url.find(':')));
    if (scheme != "http" && scheme != "https") {
        return std::string();
    }
    return final_url;
}

const BrowserFetchResult *BrowserApp::find_cached_resource(const std::string &url) const
{
    for (const CachedResource &entry : resource_cache_) {
        if (entry.url == url) {
            return &entry.result;
        }
    }
    return nullptr;
}

void BrowserApp::store_cached_resource(const std::string &url, const BrowserFetchResult &result)
{
    for (CachedResource &entry : resource_cache_) {
        if (entry.url == url) {
            entry.result = result;
            return;
        }
    }

    constexpr std::size_t kMaxCachedResources = 24;
    if (resource_cache_.size() >= kMaxCachedResources) {
        resource_cache_.erase(resource_cache_.begin());
    }

    CachedResource entry;
    entry.url = url;
    entry.result = result;
    resource_cache_.push_back(std::move(entry));
}

const BrowserApp::CachedImage *BrowserApp::find_cached_image(const std::string &url) const
{
    for (const CachedImage &entry : image_cache_) {
        if (entry.url == url) {
            return &entry;
        }
    }
    return nullptr;
}

BrowserFetchResult BrowserApp::fetch_resource(const std::string &url)
{
    if (const BrowserFetchResult *cached = find_cached_resource(url)) {
        return *cached;
    }

    BrowserFetchResult inline_result;
    if (decode_data_url(url, inline_result)) {
        if (inline_result.ok) {
            store_cached_resource(url, inline_result);
        }
        return inline_result;
    }

    BrowserFetchResult result = browser_fetch_url(url);
    if (result.ok) {
        const std::string cache_key = result.final_url.empty() ? url : result.final_url;
        store_cached_resource(cache_key, result);
        if (cache_key != url) {
            store_cached_resource(url, result);
        }
    }
    return result;
}

const BrowserDecodedImage *BrowserApp::load_image_resource(const std::string &url)
{
    if (url.empty()) {
        return nullptr;
    }

    if (const CachedImage *cached = find_cached_image(url)) {
        return cached->failed ? nullptr : &cached->image;
    }

    constexpr std::size_t kMaxCachedImages = 16;
    if (image_cache_.size() >= kMaxCachedImages) {
        image_cache_.erase(image_cache_.begin());
    }

    CachedImage entry;
    entry.url = url;

    BrowserFetchResult response = fetch_resource(url);
    if (!response.ok) {
        entry.failed = true;
        image_cache_.push_back(std::move(entry));
        log_line("图片加载失败：" + shorten_url_for_log(url) + " 错误=" + response.error +
                 " 类型=" + (response.content_type.empty() ? std::string("-") : response.content_type));
        return nullptr;
    }

    std::string decode_error;
    if (!browser_decode_image(response.body, response.content_type, entry.image, decode_error)) {
        entry.failed = true;
        image_cache_.push_back(std::move(entry));
        log_line("图片解码失败：" + shorten_url_for_log(url) + " 错误=" + decode_error +
                 " 类型=" + (response.content_type.empty() ? std::string("-") : response.content_type));
        return nullptr;
    }

    char info[256];
    std::snprintf(info,
                  sizeof(info),
                  "图片就绪：%s %dx%d 类型=%s",
                  shorten_url_for_log(url).c_str(),
                  entry.image.width,
                  entry.image.height,
                  response.content_type.empty() ? "-" : response.content_type.c_str());
    log_line(info);

    image_cache_.push_back(std::move(entry));
    return &image_cache_.back().image;
}

void BrowserApp::request_navigation(const std::string &url)
{
    navigate_to(url, BrowserNavReason::Normal);
}

void BrowserApp::navigate_to(const std::string &url, BrowserNavReason reason)
{
    if (url.empty()) {
        return;
    }

    ++navigation_serial_;
    pending_nav_reason_ = reason;
    suppress_history_record_ = reason != BrowserNavReason::Normal;
    pending_url_ = url;
    std::snprintf(url_input_, sizeof(url_input_), "%s", url.c_str());
    input_len_ = static_cast<int>(std::strlen(url_input_));
    input_cursor_ = input_len_;
    load_pending_ = true;
    invalidate_header();
}

void BrowserApp::record_history(const std::string &url)
{
    if (url.empty()) {
        return;
    }

    if (history_index_ >= 0 &&
        history_index_ < static_cast<int>(history_.size()) &&
        history_[history_index_] == url) {
        current_url_ = url;
        return;
    }

    if (history_index_ + 1 < static_cast<int>(history_.size())) {
        history_.erase(history_.begin() + history_index_ + 1, history_.end());
    }

    history_.push_back(url);
    history_index_ = static_cast<int>(history_.size()) - 1;
    current_url_ = url;
}

void BrowserApp::go_history(int delta)
{
    const int next_index = history_index_ + delta;
    if (next_index < 0 || next_index >= static_cast<int>(history_.size())) {
        return;
    }
    history_index_ = next_index;
    navigate_to(history_[history_index_], BrowserNavReason::History);
}

void BrowserApp::reload_current()
{
    const std::string target = current_url_.empty() ? trim_copy(std::string(url_input_)) : current_url_;
    if (target.empty()) {
        return;
    }
    navigate_to(target, BrowserNavReason::Reload);
}

void BrowserApp::stop_loading()
{
    if (!load_pending_) {
        set_status(browser_tr("就绪", "Ready"));
        return;
    }
    load_pending_ = false;
    pending_url_.clear();
    set_status(browser_tr("已停止", "Stopped"));
}

int BrowserApp::scroll_content_height() const
{
    if (document_) {
        return static_cast<int>(document_->height());
    }
    return text_content_height_;
}

void BrowserApp::clamp_scroll()
{
    const int max_scroll = std::max(0, scroll_content_height() - content_height());
    scroll_y_ = std::clamp(scroll_y_, 0, max_scroll);
}

void BrowserApp::draw_scrollbar()
{
    const int x1 = scrollbar_x();
    const int x2 = x1 + BROWSER_SCROLLBAR_WIDTH;
    const int y1 = content_y();
    const int y2 = content_y() + content_height();
    xapi_DrawRect(window_, x1, y1, x2, y2, 0xd4dde4ff, true);

    const int total_height = scroll_content_height();
    if (total_height <= content_height() || total_height <= 0) {
        xapi_DrawRect(window_, x1 + 2, y1 + 2, x2 - 2, y2 - 2, 0x9aa8b3ff, true);
        return;
    }

    const int track_height = content_height() - 4;
    const int thumb_height = std::max(BROWSER_SCROLLBAR_MIN_THUMB, (content_height() * track_height) / total_height);
    const int max_scroll = std::max(1, total_height - content_height());
    const int thumb_y = y1 + 2 + ((track_height - thumb_height) * scroll_y_) / max_scroll;
    xapi_DrawRect(window_, x1 + 2, thumb_y, x2 - 2, thumb_y + thumb_height, COLOR_BUTTON, true);
}

bool BrowserApp::handle_scrollbar_click(int x, int y)
{
    if (x < scrollbar_x() || x >= scrollbar_x() + BROWSER_SCROLLBAR_WIDTH ||
        y < content_y() || y >= content_y() + content_height()) {
        return false;
    }

    const int total_height = scroll_content_height();
    if (total_height <= content_height()) {
        scroll_y_ = 0;
        refresh_content_view();
        return true;
    }

    const int track_height = std::max(1, content_height() - 4);
    const int thumb_height = std::max(BROWSER_SCROLLBAR_MIN_THUMB, (content_height() * track_height) / total_height);
    const int max_scroll = std::max(1, total_height - content_height());
    const int track_click = std::clamp(y - content_y() - 2 - thumb_height / 2, 0, std::max(1, track_height - thumb_height));
    scroll_y_ = (track_click * max_scroll) / std::max(1, track_height - thumb_height);
    clamp_scroll();
    refresh_content_view();
    return true;
}

void BrowserApp::prepare_text_view(const BrowserFetchResult &response)
{
    std::string text = looks_like_html(response) ? html_to_text(response.body) : response.body;
    text = trim_copy(text);
    if (text.empty()) {
        text = browser_tr("页面已加载，但没有生成可见文本内容。",
                          "The page loaded, but did not produce visible text content.");
    }

    page_text_ = text;
    text_lines_ = wrap_text_for_width(text, std::max(120, content_width() - 24));
    text_content_height_ = static_cast<int>(text_lines_.size()) * BROWSER_TEXT_LINE_HEIGHT + 24;
    update_find_matches();
}

bool BrowserApp::hit_content_area(int x, int y) const
{
    return x >= content_x() && x < content_x() + content_width() &&
           y >= content_y() && y < content_y() + content_height();
}

bool BrowserApp::hit_url_input(int x, int y) const
{
    return x >= BROWSER_INPUT_X && x < input_right() &&
           y >= BROWSER_INPUT_Y && y < BROWSER_INPUT_Y + BROWSER_INPUT_HEIGHT;
}

bool BrowserApp::hit_find_input(int x, int y) const
{
    return x >= BROWSER_FIND_X && x < find_right() &&
           y >= BROWSER_FIND_Y && y < BROWSER_FIND_Y + BROWSER_FIND_HEIGHT;
}

BrowserCommand BrowserApp::command_at(int x, int y) const
{
    const BrowserButton buttons[] = {
        {BrowserCommand::Back, 14, 16, 52, 44, "<"},
        {BrowserCommand::Forward, 58, 16, 96, 44, ">"},
        {BrowserCommand::Reload, 102, 16, 172, 44, browser_tr("刷新", "Reload")},
        {BrowserCommand::Stop, 178, 16, 242, 44, browser_tr("停止", "Stop")},
        {BrowserCommand::FindPrev, window_width_ - 132, 52, window_width_ - 76, 76, browser_tr("上个", "Prev")},
        {BrowserCommand::FindNext, window_width_ - 70, 52, window_width_ - 14, 76, browser_tr("下个", "Next")},
    };

    for (const BrowserButton &button : buttons) {
        if (x >= button.x1 && x < button.x2 && y >= button.y1 && y < button.y2) {
            return button.command;
        }
    }
    return BrowserCommand::None;
}

bool BrowserApp::command_enabled(BrowserCommand command) const
{
    switch (command) {
    case BrowserCommand::Back:
        return history_index_ > 0;
    case BrowserCommand::Forward:
        return history_index_ >= 0 && history_index_ + 1 < static_cast<int>(history_.size());
    case BrowserCommand::Reload:
        return !current_url_.empty() || input_len_ > 0;
    case BrowserCommand::Stop:
        return load_pending_;
    case BrowserCommand::FindPrev:
    case BrowserCommand::FindNext:
        return find_len_ > 0 && find_match_count_ > 0;
    default:
        return true;
    }
}

void BrowserApp::execute_command(BrowserCommand command)
{
    if (!command_enabled(command)) {
        return;
    }

    switch (command) {
    case BrowserCommand::Back:
        go_history(-1);
        break;
    case BrowserCommand::Forward:
        go_history(1);
        break;
    case BrowserCommand::Reload:
        reload_current();
        break;
    case BrowserCommand::Stop:
        stop_loading();
        break;
    case BrowserCommand::FindPrev:
        find_next(-1);
        break;
    case BrowserCommand::FindNext:
        find_next(1);
        break;
    default:
        break;
    }
}

void BrowserApp::focus_url(bool select_all)
{
    input_mode_ = BrowserInputMode::Url;
    if (select_all) {
        input_len_ = 0;
        url_input_[0] = '\0';
    }
    input_cursor_ = input_len_;
    startup_guard_ = false;
    invalidate_header();
}

void BrowserApp::focus_find(bool select_all)
{
    input_mode_ = BrowserInputMode::Find;
    if (select_all) {
        find_len_ = 0;
        find_input_[0] = '\0';
        find_match_index_ = -1;
        find_match_count_ = 0;
    }
    find_cursor_ = find_len_;
    startup_guard_ = false;
    invalidate_header();
}

void BrowserApp::insert_char(char ch)
{
    char *buffer = input_mode_ == BrowserInputMode::Url ? url_input_ : find_input_;
    int &length = input_mode_ == BrowserInputMode::Url ? input_len_ : find_len_;
    int &cursor = input_mode_ == BrowserInputMode::Url ? input_cursor_ : find_cursor_;
    const int capacity = input_mode_ == BrowserInputMode::Url ? static_cast<int>(sizeof(url_input_)) : static_cast<int>(sizeof(find_input_));

    if (length >= capacity - 1) {
        return;
    }

    for (int i = length; i >= cursor; --i) {
        buffer[i + 1] = buffer[i];
    }
    buffer[cursor++] = ch;
    ++length;
    buffer[length] = '\0';

    if (input_mode_ == BrowserInputMode::Find) {
        update_find_matches();
    }
    invalidate_header();
}

void BrowserApp::backspace_input()
{
    char *buffer = input_mode_ == BrowserInputMode::Url ? url_input_ : find_input_;
    int &length = input_mode_ == BrowserInputMode::Url ? input_len_ : find_len_;
    int &cursor = input_mode_ == BrowserInputMode::Url ? input_cursor_ : find_cursor_;

    if (cursor <= 0 || length <= 0) {
        return;
    }

    for (int i = cursor - 1; i < length; ++i) {
        buffer[i] = buffer[i + 1];
    }
    --cursor;
    --length;

    if (input_mode_ == BrowserInputMode::Find) {
        update_find_matches();
    }
    invalidate_header();
}

void BrowserApp::update_find_matches()
{
    find_match_count_ = 0;
    find_match_index_ = -1;
    const std::string needle = trim_copy(std::string(find_input_));
    if (needle.empty() || page_text_.empty()) {
        return;
    }

    std::size_t pos = 0;
    while (pos < page_text_.size()) {
        pos = find_case_insensitive(page_text_, needle, pos);
        if (pos == std::string::npos) {
            break;
        }
        ++find_match_count_;
        pos += std::max<std::size_t>(1, needle.size());
    }

    if (find_match_count_ > 0) {
        find_match_index_ = 0;
    }
}

void BrowserApp::find_next(int direction)
{
    if (find_len_ <= 0) {
        focus_find(false);
        return;
    }
    update_find_matches();
    if (find_match_count_ <= 0) {
        set_status(browser_tr("查找：无匹配", "Find: no matches"));
        return;
    }

    if (find_match_index_ < 0) {
        find_match_index_ = 0;
    } else {
        find_match_index_ = (find_match_index_ + direction + find_match_count_) % find_match_count_;
    }

    if (!document_) {
        const std::string needle = trim_copy(std::string(find_input_));
        int seen = 0;
        for (int i = 0; i < static_cast<int>(text_lines_.size()); ++i) {
            if (find_case_insensitive(text_lines_[i], needle) != std::string::npos) {
                if (seen == find_match_index_) {
                    scroll_y_ = std::max(0, i * BROWSER_TEXT_LINE_HEIGHT - 24);
                    clamp_scroll();
                    break;
                }
                ++seen;
            }
        }
    }

    char text[64];
    std::snprintf(text,
                  sizeof(text),
                  browser_tr("查找 %d/%d", "Find %d/%d"),
                  find_match_index_ + 1,
                  find_match_count_);
    set_status(text);
    refresh_content_view();
}

std::string BrowserApp::clipped_tail_for_width(const std::string &text, int width) const
{
    if (width <= 0 || text.empty()) {
        return std::string();
    }

    std::string visible = text;
    while (!visible.empty() &&
           xapi_CalcTextWidth(const_cast<char *>(visible.c_str()), 10) > static_cast<unsigned long long>(width)) {
        visible.erase(visible.begin());
    }
    return visible;
}

void BrowserApp::draw_text_clipped(int x, int y, int width, const std::string &text, std::uint32_t color)
{
    const std::string visible = clipped_tail_for_width(text, width);
    if (visible.empty()) {
        return;
    }

    xapi_DrawSWText(window_, static_cast<unsigned int>(x), static_cast<unsigned int>(y), const_cast<char *>(visible.c_str()), color);
}

int BrowserApp::text_width_px(const std::string &text, unsigned int size) const
{
    if (text.empty()) {
        return 0;
    }
    return static_cast<int>(xapi_CalcTextWidth(const_cast<char *>(text.c_str()), size));
}

void BrowserApp::draw_input_text(int x, int y, int width, const std::string &text, std::uint32_t color)
{
    const std::string visible = clipped_tail_for_width(text, width);
    if (visible.empty()) {
        return;
    }

    xapi_DrawText(window_,
                  static_cast<unsigned int>(x),
                  static_cast<unsigned int>(y),
                  const_cast<char *>(visible.c_str()),
                  10,
                  color);
}

void BrowserApp::draw_button(const BrowserButton &button)
{
    const bool enabled = command_enabled(button.command);
    xapi_DrawRect(window_, button.x1, button.y1, button.x2, button.y2, enabled ? COLOR_BUTTON : COLOR_BUTTON_DISABLED, true);
    const int label_width = static_cast<int>(xapi_CalcTextWidth(const_cast<char *>(button.label), 10));
    const int button_width = button.x2 - button.x1;
    const int text_x = button.x1 + std::max(4, (button_width - label_width) / 2);
    draw_text_clipped(text_x, button.y1 + 6, button_width - 8, button.label, enabled ? COLOR_MUTED : 0x9aa8b3ff);
}

std::string BrowserApp::href_from_element(litehtml::element::const_ptr element) const
{
    while (element) {
        const char *href = element->get_attr("href");
        if (href == nullptr || *href == '\0') {
            element = element->parent();
            continue;
        }
        return resolve_url(href, base_url_);
    }

    return std::string();
}

void BrowserApp::process_pointer_move(int x, int y)
{
    if (x >= scrollbar_x() && x < scrollbar_x() + BROWSER_SCROLLBAR_WIDTH &&
        y >= content_y() && y < content_y() + content_height()) {
        return;
    }

    if (!document_ || !hit_content_area(x, y)) {
        if (!hovered_url_.empty()) {
            hovered_url_.clear();
            invalidate_header();
        }
        return;
    }

    litehtml::position::vector redraw;
    const int client_x = x - content_x();
    const int client_y = y - content_y();
    document_->on_mouse_over(static_cast<float>(client_x),
                             static_cast<float>(client_y + scroll_y_),
                             static_cast<float>(client_x),
                             static_cast<float>(client_y),
                             redraw);

    const std::string href = href_from_element(document_->get_over_element());
    if (href != hovered_url_) {
        hovered_url_ = href;
        invalidate_header();
    }

    if (!redraw.empty()) {
        invalidate_content();
    }
}

void BrowserApp::process_pointer_click(int x, int y)
{
    if (handle_scrollbar_click(x, y)) {
        return;
    }

    const BrowserCommand command = command_at(x, y);
    if (command != BrowserCommand::None) {
        execute_command(command);
        return;
    }

    if (hit_url_input(x, y)) {
        focus_url(true);
        return;
    }

    if (hit_find_input(x, y)) {
        focus_find(false);
        return;
    }

    if (!document_ || !hit_content_area(x, y)) {
        return;
    }

    litehtml::position::vector redraw;
    const int client_x = x - content_x();
    const int client_y = y - content_y();
    const int page_y = client_y + scroll_y_;
    const unsigned int navigation_before = navigation_serial_;
    document_->on_lbutton_down(static_cast<float>(client_x),
                               static_cast<float>(page_y),
                               static_cast<float>(client_x),
                               static_cast<float>(client_y),
                               redraw);
    document_->on_lbutton_up(static_cast<float>(client_x),
                             static_cast<float>(page_y),
                             static_cast<float>(client_x),
                             static_cast<float>(client_y),
                             redraw);

    if (navigation_serial_ == navigation_before) {
        const std::string href = href_from_element(document_->get_over_element());
        if (!href.empty()) {
            request_navigation(href);
            return;
        }
    }

    invalidate_content();
}

void BrowserApp::handle_message(unsigned long long type, unsigned long long hData, unsigned long long lData)
{
    switch (type) {
    case MSG_CHAR:
        if (static_cast<char>(lData) >= ' ') {
            startup_guard_ = false;
            insert_char(static_cast<char>(lData));
        }
        break;
    case MSG_SPCHAR:
        if (static_cast<char>(lData) == '\b') {
            startup_guard_ = false;
            backspace_input();
        } else if (static_cast<char>(lData) == '\n') {
            if (input_mode_ == BrowserInputMode::Find) {
                find_next(1);
            } else if (!startup_guard_) {
                navigate_to(url_input_, BrowserNavReason::Normal);
            } else {
                startup_guard_ = false;
            }
        } else if (static_cast<unsigned char>(lData) == BROWSER_KEY_F3) {
            find_next(1);
        }
        break;
    case MSG_MOVE:
        process_pointer_move(static_cast<int>(hData), static_cast<int>(lData));
        break;
    case MSG_LBUTTON:
        process_pointer_click(static_cast<int>(hData), static_cast<int>(lData));
        break;
    case MSG_ROLLER:
        {
            const int old_scroll = scroll_y_;
            scroll_y_ -= static_cast<int>(hData) * BROWSER_SCROLL_STEP;
            clamp_scroll();
            if (scroll_y_ != old_scroll) {
                refresh_content_view();
            }
        }
        break;
    case MSG_RESIZE:
        window_width_ = static_cast<int>(hData);
        window_height_ = static_cast<int>(lData);
        if (document_) {
            container_.set_viewport(content_width(), content_height());
            document_->render(static_cast<float>(content_width()));
        } else if (!page_text_.empty()) {
            text_lines_ = wrap_text_for_width(page_text_, std::max(120, content_width() - 24));
            text_content_height_ = static_cast<int>(text_lines_.size()) * BROWSER_TEXT_LINE_HEIGHT + 24;
        }
        clamp_scroll();
        invalidate_all();
        break;
    default:
        break;
    }
}

void BrowserApp::load_current_url()
{
    load_pending_ = false;
    set_status(browser_tr("正在加载...", "Loading..."));
    render();

    std::string target = pending_url_.empty() ? std::string(url_input_) : pending_url_;
    try {
        BrowserFetchResult response;
        BrowserPreparedDocument prepared;

        constexpr int kMaxCompatRedirects = 3;
        int compat_redirects = 0;
        while (true) {
            log_line(std::string(browser_tr("开始获取 URL=", "Fetching URL=")) + target);
            response = fetch_resource(target);
            if (!response.ok) {
                log_line(std::string(browser_tr("加载失败：", "Load failed: ")) + response.error);
                document_.reset();
                base_url_ = target;
                scroll_y_ = 0;
                set_status(browser_tr("加载失败", "Load failed"));
                invalidate_content();
                return;
            }

            base_url_ = response.final_url.empty() ? target : response.final_url;
            current_url_ = base_url_;

            char info[256];
            std::snprintf(info,
                          sizeof(info),
                          browser_tr("获取成功 状态=%d 字节=%llu 类型=%s 字符集=%s",
                                     "Fetch ok status=%d bytes=%llu type=%s charset=%s"),
                          response.status_code,
                          static_cast<unsigned long long>(response.body.size()),
                          response.content_type.empty() ? "-" : response.content_type.c_str(),
                          response.charset.empty() ? "-" : response.charset.c_str());
            log_line(info);

            prepared = BrowserPreparedDocument();
            if (BROWSER_ENABLE_LITEHTML) {
                prepared = prepare_document_html(response);
                if (prepared.script_blocks > 0) {
                    char js_info[256];
                    std::snprintf(js_info,
                                  sizeof(js_info),
                                  browser_tr("JS 兼容：脚本=%d 外部=%d 写入=%d 生效=%d",
                                             "JS compat: scripts=%d external=%d writes=%d effects=%d"),
                                  prepared.script_blocks,
                                  prepared.external_scripts,
                                  prepared.document_writes,
                                  prepared.scripts_with_effect);
                    log_line(js_info);
                }
            }

            if (!prepared.redirect_url.empty() &&
                prepared.redirect_url != target &&
                compat_redirects < kMaxCompatRedirects) {
                log_line(std::string(browser_tr("JS 重定向：", "JS redirect: ")) +
                         shorten_url_for_log(target) + " -> " + shorten_url_for_log(prepared.redirect_url));
                target = prepared.redirect_url;
                ++compat_redirects;
                continue;
            }
            break;
        }

        page_title_.clear();
        scroll_y_ = 0;
        document_.reset();
        text_lines_.clear();
        page_text_.clear();
        text_content_height_ = 0;

        bool prepared_fallback = !BROWSER_ENABLE_LITEHTML || prepared.fallback_used;
        if (!prepared_fallback) {
            litehtml::estring html(prepared.html);
            html.encoding = litehtml::encoding::utf_8;
            html.confidence = litehtml::confidence::certain;
            if (!response.charset.empty()) {
                litehtml::encoding declared_encoding = litehtml::get_encoding(lower_copy(response.charset));
                if (declared_encoding != litehtml::encoding::null) {
                    html.encoding = declared_encoding;
                }
            }

            container_.set_viewport(content_width(), content_height());
            log_line(browser_tr("开始创建文档", "Creating document"));
            document_ = litehtml::document::createFromString(html, &container_, litehtml::master_css, BROWSER_USER_CSS);
            if (document_) {
                log_line(browser_tr("开始渲染文档", "Rendering document"));
                document_->render(static_cast<float>(content_width()));
                page_text_.clear();
                if (document_->root()) {
                    document_->root()->get_text(page_text_);
                    page_text_ = trim_copy(page_text_);
                    update_find_matches();
                }
                log_line(browser_tr("文档渲染完成", "Document rendered"));
            } else {
                prepared_fallback = true;
            }
        }

        if (prepared_fallback) {
            if (!BROWSER_ENABLE_LITEHTML) {
                log_line(browser_tr("litehtml 已禁用：启用安全文本模式",
                                    "litehtml is disabled: using safe text mode"));
            }
            prepare_text_view(response);
            log_line(browser_tr("安全文本渲染就绪", "Safe text rendering is ready"));
        }
        clamp_scroll();
        set_status(browser_tr("已加载", "Loaded"));
        if (!suppress_history_record_) {
            record_history(base_url_);
        } else {
            current_url_ = base_url_;
            suppress_history_record_ = false;
        }
        invalidate_content();
    } catch (const std::exception &ex) {
        log_line(std::string(browser_tr("异常：", "Exception: ")) + ex.what());
        document_.reset();
        set_status(browser_tr("异常", "Exception"));
        invalidate_content();
    } catch (...) {
        log_line(browser_tr("未知异常", "Unknown exception"));
        document_.reset();
        set_status(browser_tr("异常", "Exception"));
        invalidate_content();
    }
}

void BrowserApp::render_frame()
{
    xapi_DrawRect(window_, 0, 0, window_width_ - 1, window_height_ - 1, COLOR_BG, true);
    xapi_DrawRect(window_, 0, 0, window_width_ - 1, BROWSER_HEADER_HEIGHT - 1, COLOR_HEADER, true);
    xapi_DrawRect(window_,
                  BROWSER_MARGIN,
                  BROWSER_HEADER_HEIGHT + 2,
                  window_width_ - BROWSER_MARGIN,
                  window_height_ - BROWSER_MARGIN,
                  COLOR_PANEL,
                  true);
    xapi_DrawRect(window_,
                  BROWSER_INPUT_X,
                  BROWSER_INPUT_Y,
                  input_right(),
                  BROWSER_INPUT_Y + BROWSER_INPUT_HEIGHT,
                  COLOR_INPUT,
                  true);
    xapi_DrawRect(window_,
                  BROWSER_FIND_X,
                  BROWSER_FIND_Y,
                  find_right(),
                  BROWSER_FIND_Y + BROWSER_FIND_HEIGHT,
                  COLOR_INPUT,
                  true);
}

void BrowserApp::render_header()
{
    xapi_DrawRect(window_, 0, 0, window_width_ - 1, BROWSER_HEADER_HEIGHT - 1, COLOR_HEADER, true);
    xapi_DrawRect(window_,
                  BROWSER_INPUT_X,
                  BROWSER_INPUT_Y,
                  input_right(),
                  BROWSER_INPUT_Y + BROWSER_INPUT_HEIGHT,
                  COLOR_INPUT,
                  true);
    xapi_DrawRect(window_,
                  BROWSER_FIND_X,
                  BROWSER_FIND_Y,
                  find_right(),
                  BROWSER_FIND_Y + BROWSER_FIND_HEIGHT,
                  COLOR_INPUT,
                  true);

    const BrowserButton buttons[] = {
        {BrowserCommand::Back, 14, 16, 52, 44, "<"},
        {BrowserCommand::Forward, 58, 16, 96, 44, ">"},
        {BrowserCommand::Reload, 102, 16, 172, 44, browser_tr("刷新", "Reload")},
        {BrowserCommand::Stop, 178, 16, 242, 44, browser_tr("停止", "Stop")},
        {BrowserCommand::FindPrev, window_width_ - 132, 52, window_width_ - 76, 76, browser_tr("上个", "Prev")},
        {BrowserCommand::FindNext, window_width_ - 70, 52, window_width_ - 14, 76, browser_tr("下个", "Next")},
    };
    for (const BrowserButton &button : buttons) {
        draw_button(button);
    }

    xapi_DrawSWText(window_, 20, 56, const_cast<char *>(status_), COLOR_STATUS);
    xapi_DrawSWText(window_, BROWSER_INPUT_X - 34, 20, const_cast<char *>("URL"), COLOR_MUTED);
    xapi_DrawSWText(window_, BROWSER_FIND_X - 48, 56, browser_tr("查找", "Find"), COLOR_MUTED);
    const int url_text_width = input_right() - BROWSER_INPUT_X - 16;
    const int find_text_width = find_right() - BROWSER_FIND_X - 16;
    const std::string visible_url = clipped_tail_for_width(url_input_, url_text_width);
    const std::string visible_find = clipped_tail_for_width(find_input_, find_text_width);
    draw_input_text(BROWSER_INPUT_X + 8, BROWSER_INPUT_Y + 3, url_text_width, url_input_, COLOR_TEXT);
    draw_input_text(BROWSER_FIND_X + 8, BROWSER_FIND_Y + 1, find_text_width, find_input_, COLOR_TEXT);

    if (input_mode_ == BrowserInputMode::Url) {
        const int cursor_in_visible = std::clamp(input_cursor_ - (input_len_ - static_cast<int>(visible_url.size())), 0, static_cast<int>(visible_url.size()));
        const std::string cursor_prefix = visible_url.substr(0, static_cast<std::size_t>(cursor_in_visible));
        const int cursor_x = std::min(input_right() - 6, BROWSER_INPUT_X + 8 + text_width_px(cursor_prefix, 10));
        xapi_DrawLine(window_, cursor_x, BROWSER_INPUT_Y + 5, cursor_x, BROWSER_INPUT_Y + BROWSER_INPUT_HEIGHT - 5, COLOR_LINK);
    } else {
        const int cursor_in_visible = std::clamp(find_cursor_ - (find_len_ - static_cast<int>(visible_find.size())), 0, static_cast<int>(visible_find.size()));
        const std::string cursor_prefix = visible_find.substr(0, static_cast<std::size_t>(cursor_in_visible));
        const int cursor_x = std::min(find_right() - 6, BROWSER_FIND_X + 8 + text_width_px(cursor_prefix, 10));
        xapi_DrawLine(window_, cursor_x, BROWSER_FIND_Y + 4, cursor_x, BROWSER_FIND_Y + BROWSER_FIND_HEIGHT - 4, COLOR_LINK);
    }

    char find_info[32];
    if (find_len_ > 0) {
        if (find_match_count_ > 0) {
            std::snprintf(find_info, sizeof(find_info), "%d/%d", std::max(1, find_match_index_ + 1), find_match_count_);
        } else {
            std::snprintf(find_info, sizeof(find_info), "%s", "0/0");
        }
        xapi_DrawSWText(window_, find_right() + 8, BROWSER_FIND_Y + 4, find_info, COLOR_STATUS);
    }

    const std::string title = page_title_.empty() ? std::string(browser_tr("litehtml 浏览器", "litehtml Browser")) : page_title_;
    xapi_DrawRect(window_, window_width_ - 136, 16, window_width_ - BROWSER_MARGIN, 44, COLOR_HEADER, true);
    draw_text_clipped(window_width_ - 132, 20, 110, title, COLOR_MUTED);

    if (!hovered_url_.empty()) {
        draw_text_clipped(180, 82, window_width_ - 200, hovered_url_, COLOR_STATUS);
    }
}

void BrowserApp::render_content()
{
    xapi_DrawRect(window_,
                  content_x(),
                  content_y(),
                  content_x() + content_width() + BROWSER_SCROLLBAR_WIDTH,
                  content_y() + content_height(),
                  COLOR_PANEL,
                  true);

    if (document_) {
        try {
            container_.set_viewport(content_width(), content_height());
            litehtml::position clip(static_cast<float>(content_x()),
                                    static_cast<float>(content_y()),
                                    static_cast<float>(content_width()),
                                    static_cast<float>(content_height()));
            document_->draw(window_, static_cast<float>(content_x()), static_cast<float>(content_y() - scroll_y_), &clip);
        } catch (const std::exception &ex) {
            log_line(std::string(browser_tr("绘制异常：", "Draw exception: ")) + ex.what());
            document_.reset();
            set_status(browser_tr("绘制异常", "Draw exception"));
        } catch (...) {
            log_line(browser_tr("绘制未知异常", "Unknown draw exception"));
            document_.reset();
            set_status(browser_tr("绘制异常", "Draw exception"));
        }
    } else if (!text_lines_.empty()) {
        const int start_line = std::max(0, scroll_y_ / BROWSER_TEXT_LINE_HEIGHT);
        const int visible_lines = content_height() / BROWSER_TEXT_LINE_HEIGHT + 2;
        for (int i = 0; i < visible_lines; ++i) {
            const int line_index = start_line + i;
            if (line_index < 0 || line_index >= static_cast<int>(text_lines_.size())) {
                break;
            }
            const int y = content_y() + 12 + i * BROWSER_TEXT_LINE_HEIGHT - (scroll_y_ % BROWSER_TEXT_LINE_HEIGHT);
            if (y >= content_y() + content_height()) {
                break;
            }
            xapi_DrawSWText(window_,
                            content_x() + 12,
                            static_cast<unsigned int>(y),
                            const_cast<char *>(text_lines_[line_index].c_str()),
                            COLOR_TEXT);
        }
    } else {
        xapi_DrawSWText(window_,
                        content_x() + 12,
                        content_y() + 12,
                        browser_tr("尚未加载页面。", "No page loaded."),
                        COLOR_ERROR);
    }

    draw_scrollbar();
}

void BrowserApp::refresh_content_view()
{
    render_content();
    xapi_RefreshPartWindow(window_,
                           content_x(),
                           content_y(),
                           content_x() + content_width() + BROWSER_SCROLLBAR_WIDTH,
                           content_y() + content_height());
    content_dirty_ = false;
    need_redraw_ = header_dirty_ || frame_dirty_;
}

void BrowserApp::render()
{
    if (frame_dirty_) {
        render_frame();
        render_header();
        render_content();
        xapi_RefreshPartWindow(window_, 0, 0, window_width_, window_height_);
        frame_dirty_ = false;
        header_dirty_ = false;
        content_dirty_ = false;
        need_redraw_ = false;
        return;
    }

    if (header_dirty_) {
        render_header();
        xapi_RefreshPartWindow(window_, 0, 0, window_width_, BROWSER_HEADER_HEIGHT);
        header_dirty_ = false;
    }

    if (content_dirty_) {
        render_content();
        xapi_RefreshPartWindow(window_,
                               content_x(),
                               content_y(),
                               content_x() + content_width() + BROWSER_SCROLLBAR_WIDTH,
                               content_y() + content_height());
        content_dirty_ = false;
    }

    need_redraw_ = false;
}

int BrowserApp::run()
{
    g_app = this;

    XWINDOW window {};
    window.title = browser_tr("浏览器", "Browser");
    window.width = BROWSER_WIDTH;
    window.height = BROWSER_HEIGHT;
    window.sets = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;

    xapi_CreateWindow(&window_, &window);
    xapi_SetIcon(window_, const_cast<char *>("/system/icon/terminal.png"));
    SetMsgPrcor(window_, [](unsigned long long type, unsigned long long hData, unsigned long long lData) {
        if (g_app != nullptr) {
            g_app->handle_message(type, hData, lData);
        }
    });

    render();

    while (true) {
        if (load_pending_) {
            load_current_url();
        }
        if (need_redraw_) {
            render();
        }
        xapi_Sleep(BROWSER_LOOP_SLEEP_MS);
    }

    return 0;
}

} // namespace

static int browser_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    BrowserApp app;
    return app.run();
}

extern "C" int browser_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int browser_main_cpp(int argc, char *argv[], char *envp[])
{
    return browser_main_impl(argc, argv, envp);
}
