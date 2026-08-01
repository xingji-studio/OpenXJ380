#pragma once

#include "browser_platform.h"

#include <string>
#include <vector>

struct BrowserDecodedImage
{
    int                  width = 0;
    int                  height = 0;
    std::vector<XCOLORA> pixels;

    bool empty() const
    {
        return width <= 0 || height <= 0 || pixels.empty();
    }
};

bool browser_decode_image(const std::string &encoded,
                          const std::string &content_type,
                          BrowserDecodedImage &decoded,
                          std::string &error);
