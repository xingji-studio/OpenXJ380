#include "../includes/sytle.hpp"
Sytel::Sytel()
    : color_enabled(false),
      size_enabled(false),
      background_color_enabled(false),
      border_color_enabled(false),
      border_width_enabled(false),
      radius_enabled(false),
      padding_enabled(false),
      color(0),
      size(0),
      background_color(0),
      border_color(0),
      border_width(0),
      radius(0),
      padding(0) {}

Sytel::~Sytel() = default;

void Sytel::set_color(unsigned int color) {
    this->color = color;
    this->color_enabled = true;
}

void Sytel::set_size(unsigned int size) {
    this->size = size;
    this->size_enabled = true;
}

void Sytel::set_background_color(unsigned int color) {
    this->background_color = color;
    this->background_color_enabled = true;
}

void Sytel::set_border_color(unsigned int color) {
    this->border_color = color;
    this->border_color_enabled = true;
}

void Sytel::set_border_width(unsigned int width) {
    this->border_width = width;
    this->border_width_enabled = true;
}

void Sytel::set_radius(unsigned int radius) {
    this->radius = radius;
    this->radius_enabled = true;
}

void Sytel::set_padding(unsigned int padding) {
    this->padding = padding;
    this->padding_enabled = true;
}

void Sytel::unset_color() {
    this->color_enabled = false;
}

void Sytel::unset_size() {
    this->size_enabled = false;
}

void Sytel::unset_background_color() {
    this->background_color_enabled = false;
}

void Sytel::unset_border_color() {
    this->border_color_enabled = false;
}

void Sytel::unset_border_width() {
    this->border_width_enabled = false;
}

void Sytel::unset_radius() {
    this->radius_enabled = false;
}

void Sytel::unset_padding() {
    this->padding_enabled = false;
}

bool Sytel::has_color() const {
    return this->color_enabled;
}

bool Sytel::has_size() const {
    return this->size_enabled;
}

bool Sytel::has_background_color() const {
    return this->background_color_enabled;
}

bool Sytel::has_border_color() const {
    return this->border_color_enabled;
}

bool Sytel::has_border_width() const {
    return this->border_width_enabled;
}

bool Sytel::has_radius() const {
    return this->radius_enabled;
}

bool Sytel::has_padding() const {
    return this->padding_enabled;
}

 bool Sytel::empty() const {
    return !this->color_enabled &&
           !this->size_enabled &&
           !this->background_color_enabled &&
           !this->border_color_enabled &&
           !this->border_width_enabled &&
           !this->radius_enabled &&
           !this->padding_enabled;
}

unsigned int Sytel::get_color(unsigned int fallback) const {
    return this->color_enabled ? this->color : fallback;
}

unsigned int Sytel::get_size(unsigned int fallback) const {
    return this->size_enabled ? this->size : fallback;
}

unsigned int Sytel::get_background_color(unsigned int fallback) const {
    return this->background_color_enabled ? this->background_color : fallback;
}

unsigned int Sytel::get_border_color(unsigned int fallback) const {
    return this->border_color_enabled ? this->border_color : fallback;
}

unsigned int Sytel::get_border_width(unsigned int fallback) const {
    return this->border_width_enabled ? this->border_width : fallback;
}

unsigned int Sytel::get_radius(unsigned int fallback) const {
    return this->radius_enabled ? this->radius : fallback;
}

unsigned int Sytel::get_padding(unsigned int fallback) const {
    return this->padding_enabled ? this->padding : fallback;
}

void Sytel::clear() {
    this->unset_color();
    this->unset_size();
    this->unset_background_color();
    this->unset_border_color();
    this->unset_border_width();
    this->unset_radius();
    this->unset_padding();
}

void Sytel::merge_from(const Sytel& sytel) {
   if (sytel.has_color()) {
        this->set_color(sytel.get_color());
    }
    if (sytel.has_size()) {
        this->set_size(sytel.get_size());
    }
    if (sytel.has_background_color()) {
        this->set_background_color(sytel.get_background_color());
    }
    if (sytel.has_border_color()) {
        this->set_border_color(sytel.get_border_color());
    }
    if (sytel.has_border_width()) {
        this->set_border_width(sytel.get_border_width());
    }
    if (sytel.has_radius()) {
        this->set_radius(sytel.get_radius());
    }
    if (sytel.has_padding()) {
        this->set_padding(sytel.get_padding());
    }
}

SytelRules::SytelRules() = default;
SytelRules::~SytelRules() = default;

void SytelRules::set_base_sytel(const Sytel& sytel) {
    this->base_sytel = sytel;
}

void SytelRules::set_on_mouse_sytel(const Sytel& sytel) {
    this->on_mouse_sytel = sytel;
}

void SytelRules::set_on_click_sytel(const Sytel& sytel) {
    this->on_click_sytel = sytel;
}

void SytelRules::set_on_hover_sytel(const Sytel& sytel) {
    this->on_hover_sytel = sytel;
}

void SytelRules::unset_base_sytel() {
    this->base_sytel.clear();
}

void SytelRules::unset_on_mouse_sytel() {
    this->on_mouse_sytel.clear();
}

void SytelRules::unset_on_click_sytel() {
    this->on_click_sytel.clear();
}

void SytelRules::unset_on_hover_sytel() {
    this->on_hover_sytel.clear();
}

bool SytelRules::has_base_sytel() const {
    return !this->base_sytel.empty();
}

bool SytelRules::has_on_mouse_sytel() const {
    return !this->on_mouse_sytel.empty();
}

bool SytelRules::has_on_click_sytel() const {
    return !this->on_click_sytel.empty();
}

bool SytelRules::has_on_hover_sytel() const {
    return !this->on_hover_sytel.empty();
}

bool SytelRules::has_any_sytel() const {
    return this->has_base_sytel() ||
           this->has_on_mouse_sytel() ||
           this->has_on_click_sytel() ||
           this->has_on_hover_sytel();
}

const Sytel& SytelRules::get_base_sytel() const {
    return this->base_sytel;
}

const Sytel& SytelRules::get_on_mouse_sytel() const {
    return this->on_mouse_sytel;
}

const Sytel& SytelRules::get_on_click_sytel() const {
    return this->on_click_sytel;
}

const Sytel& SytelRules::get_on_hover_sytel() const {
    return this->on_hover_sytel;
}

Sytel SytelRules::resolve(bool on_mouse, bool on_click, bool on_hover) const {
    Sytel result = this->base_sytel;

    if (on_hover) {
        result.merge_from(this->on_hover_sytel);
    }
    if (on_mouse) {
        result.merge_from(this->on_mouse_sytel);
    }
    if (on_click) {
        result.merge_from(this->on_click_sytel);
    }

    return result;
}

void SytelRules::clear() {
    this->base_sytel.clear();
    this->on_mouse_sytel.clear();
    this->on_click_sytel.clear();
    this->on_hover_sytel.clear();
}
