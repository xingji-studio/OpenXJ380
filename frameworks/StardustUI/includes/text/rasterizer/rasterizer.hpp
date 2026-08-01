#pragma once

#include "../../vector.hpp"
#include "geometry.hpp"

namespace stardustui::text {

class Rasterizer {
public:
    Rasterizer();
    Rasterizer(int width, int height);

    void reset(int width, int height);
    void clear();
    int width() const;
    int height() const;

    void draw_line(Point p0, Point p1);
    void draw_quad(Point p0, Point p1, Point p2);
    void draw_cubic(Point p0, Point p1, Point p2, Point p3);

    const stardustui::vector<float>& coverage() const;

private:
    int bitmap_width;
    int bitmap_height;
    stardustui::vector<float> pixels;
};

}
