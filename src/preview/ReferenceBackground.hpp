#pragma once

#include "ImageIO.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace medicon::preview {

// Deterministic pixel scenery for visual QA; deliberately independent of the HUD.
inline Image reference_background(int width, int height, bool night) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const auto put = [&](int x, int y, int r, int g, int b) {
        const auto i = (static_cast<std::size_t>(y) * width + x) * 4;
        image.rgba[i] = static_cast<unsigned char>(std::clamp(r, 0, 255));
        image.rgba[i + 1] = static_cast<unsigned char>(std::clamp(g, 0, 255));
        image.rgba[i + 2] = static_cast<unsigned char>(std::clamp(b, 0, 255));
        image.rgba[i + 3] = 255;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float v = static_cast<float>(y) / height;
            if (night) put(x, y, 3 + static_cast<int>(v * 7), 5 + static_cast<int>(v * 9), 14 + static_cast<int>(v * 17));
            else put(x, y, 58 + static_cast<int>(v * 112), 139 + static_cast<int>(v * 68), 229 + static_cast<int>(v * 13));
        }
    }
    const auto rect = [&](float l, float t, float r, float b, int red, int green, int blue) {
        const int x0 = std::clamp(static_cast<int>(l * width / 480), 0, width);
        const int x1 = std::clamp(static_cast<int>(r * width / 480), 0, width);
        const int y0 = std::clamp(static_cast<int>(t * height / 320), 0, height);
        const int y1 = std::clamp(static_cast<int>(b * height / 320), 0, height);
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) put(x, y, red, green, blue);
    };
    rect(56, 40, 86, 70, night ? 225 : 255, night ? 229 : 250, night ? 255 : 229);
    if (night) {
        for (const auto [x, y] : {std::pair{62, 22}, {117, 58}, {183, 36}, {269, 41}, {207, 85},
                               {48, 134}, {91, 140}, {149, 118}, {237, 129}, {285, 151}}) {
            rect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + 1), static_cast<float>(y + 2), 146, 154, 176);
        }
    }
    const int cr = night ? 16 : 245, cg = night ? 23 : 251, cb = night ? 44 : 255;
    rect(248, 36, 382, 64, cr, cg, cb);
    rect(224, 44, 480, 60, cr, cg, cb);
    rect(288, 60, 468, 72, cr, cg, cb);
    rect(50, 88, 130, 104, cr, cg, cb);
    rect(30, 96, 166, 108, cr, cg, cb);
    rect(288, 124, 396, 134, cr, cg, cb);
    for (int column = 0; column < 60; ++column) {
        const float top = 207.0F + 13.0F * std::sin(column * 0.61F) + 8.0F * std::sin(column * 1.13F);
        rect(column * 8.0F, top, (column + 1) * 8.0F, 286, night ? 12 : 141, night ? 21 : 182, night ? 38 : 212);
    }
    rect(0, 248, 112, 284, night ? 8 : 121, night ? 24 : 163, night ? 32 : 143);
    rect(128, 248, 264, 284, night ? 8 : 121, night ? 24 : 163, night ? 32 : 143);
    rect(312, 248, 440, 284, night ? 8 : 121, night ? 24 : 163, night ? 32 : 143);
    rect(12, 222, 20, 282, night ? 14 : 106, night ? 19 : 78, night ? 25 : 44);
    rect(0, 200, 40, 236, night ? 9 : 55, night ? 25 : 115, night ? 32 : 42);
    rect(0, 192, 28, 221, night ? 9 : 62, night ? 28 : 128, night ? 35 : 47);
    rect(416, 216, 424, 282, night ? 14 : 106, night ? 19 : 78, night ? 25 : 44);
    rect(384, 180, 448, 212, night ? 9 : 35, night ? 25 : 87, night ? 32 : 30);
    rect(392, 172, 440, 200, night ? 9 : 35, night ? 28 : 87, night ? 35 : 30);
    rect(0, 280, 480, 286, night ? 8 : 93, night ? 25 : 152, night ? 33 : 66);
    rect(0, 286, 480, 320, night ? 7 : 120, night ? 13 : 87, night ? 23 : 55);
    return image;
}

} // namespace medicon::preview
