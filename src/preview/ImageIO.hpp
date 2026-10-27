#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace medicon::preview {

// 8-bit RGBA, rows top to bottom.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    std::uint8_t* pixel(int x, int y) { return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4; }
    const std::uint8_t* pixel(int x, int y) const { return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4; }
};

// WIC-based PNG I/O. Throw std::runtime_error on failure.
void save_png(const std::filesystem::path& path, const Image& image);
Image load_image(const std::filesystem::path& path);

} // namespace medicon::preview
