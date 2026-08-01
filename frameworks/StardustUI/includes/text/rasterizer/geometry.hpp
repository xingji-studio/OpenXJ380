#pragma once

namespace stardustui::text {

struct Point {
    float x;
    float y;
};

inline Point point(float x, float y)
{
    Point value{};
    value.x = x;
    value.y = y;
    return value;
}

}
