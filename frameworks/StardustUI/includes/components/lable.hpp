#pragma once
#include "base.hpp"
#include "../string.hpp"
class Lable : public base_component
{
public:
    Lable(const stardustui::string& text, unsigned int size, unsigned int color);
    Lable(const stardustui::string& text, unsigned int size, const SytelRules& style);
    ~Lable() override;
    void draw(unsigned long long handle) override;
    bool contains(int x, int y) const override;
    int get_preferred_width() const override;
    int get_preferred_height() const override;
    void set_text(const stardustui::string& text);  
    const stardustui::string& get_text() const;
private:
    stardustui::string text;
    unsigned int size;
    unsigned int color;
};
