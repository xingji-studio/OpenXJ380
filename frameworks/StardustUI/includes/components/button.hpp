#pragma once
#include "base.hpp"
#include "../string.hpp"

class Button : public base_component
{
public:
    Button(const stardustui::string& text, int width, int height);
    Button(const stardustui::string& text, int width, int height, const SytelRules& style);
    ~Button() override;

    void draw(unsigned long long handle) override;
    int get_preferred_width() const override;
    int get_preferred_height() const override;
    void set_text(const stardustui::string& text);
    const stardustui::string& get_text() const;

private:
    stardustui::string text;
};
