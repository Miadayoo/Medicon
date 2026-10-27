// Reference images for the panel drop shadow (spec 5.5) and comparison with
// Preview screenshots. Deliberately independent of the GUI's analytic model:
// the shape mask is rasterised at 4x supersampling, box-downsampled, convolved
// with a pixel-integrated discrete Gaussian, offset, multiplied by opacity,
// masked to the outside of the shape and composited on white (sRGB gamma
// space, CSS box-shadow semantics, blur radius = 2 sigma).
//
// Usage:
//   medicon_shadow_reference --size 1920x1080 --scale 1.0 --panel x,y,w,h [--panel ...]
//       [--sigma 7] [--offset 4] [--opacity 0.38] [--radius 5]   (rpx, scaled by --scale)
//       [--out reference.png] [--csv profile.csv]
//       [--shot screenshot.png --heatmap error.png]

#include "ImageIO.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using medicon::preview::Image;

struct Rect {
    double x, y, w, h;
};

double phi(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

double rounded_sdf(const Rect& r, double radius, double px, double py) {
    const double cx = r.x + r.w * 0.5;
    const double cy = r.y + r.h * 0.5;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5);
    const double qx = std::fabs(px - cx) - (r.w * 0.5 - radius);
    const double qy = std::fabs(py - cy) - (r.h * 0.5 - radius);
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
}

// Anti-aliased coverage of the shape per pixel, 4x4 supersampled.
std::vector<double> coverage(int width, int height, const std::vector<Rect>& rects, double radius, double dy) {
    std::vector<double> mask(static_cast<std::size_t>(width) * height, 0.0);
    constexpr int ss = 4;
    for (const auto& rect : rects) {
        Rect r = rect;
        r.y += dy;
        const int x0 = std::max(0, static_cast<int>(std::floor(r.x)) - 1);
        const int x1 = std::min(width, static_cast<int>(std::ceil(r.x + r.w)) + 1);
        const int y0 = std::max(0, static_cast<int>(std::floor(r.y)) - 1);
        const int y1 = std::min(height, static_cast<int>(std::ceil(r.y + r.h)) + 1);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                int inside = 0;
                for (int sy = 0; sy < ss; ++sy) {
                    for (int sx = 0; sx < ss; ++sx) {
                        if (rounded_sdf(r, radius, x + (sx + 0.5) / ss, y + (sy + 0.5) / ss) <= 0.0) ++inside;
                    }
                }
                auto& m = mask[static_cast<std::size_t>(y) * width + x];
                m = std::max(m, inside / static_cast<double>(ss * ss));
            }
        }
    }
    return mask;
}

std::vector<double> blur(const std::vector<double>& input, int width, int height, double sigma) {
    const int radius = static_cast<int>(std::ceil(4.0 * sigma));
    std::vector<double> kernel(static_cast<std::size_t>(2 * radius + 1));
    for (int k = -radius; k <= radius; ++k) kernel[static_cast<std::size_t>(k + radius)] = phi((k + 0.5) / sigma) - phi((k - 0.5) / sigma);
    std::vector<double> temp(input.size(), 0.0);
    std::vector<double> output(input.size(), 0.0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double sum = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sx = x + k;
                if (sx >= 0 && sx < width) sum += kernel[static_cast<std::size_t>(k + radius)] * input[static_cast<std::size_t>(y) * width + sx];
            }
            temp[static_cast<std::size_t>(y) * width + x] = sum;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double sum = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int sy = y + k;
                if (sy >= 0 && sy < height) sum += kernel[static_cast<std::size_t>(k + radius)] * temp[static_cast<std::size_t>(sy) * width + x];
            }
            output[static_cast<std::size_t>(y) * width + x] = sum;
        }
    }
    return output;
}

Rect parse_rect(const std::string& text) {
    Rect r{};
    if (std::sscanf(text.c_str(), "%lf,%lf,%lf,%lf", &r.x, &r.y, &r.w, &r.h) != 4) throw std::runtime_error("--panel expects x,y,w,h");
    return r;
}

} // namespace

int main(int argc, char** argv) {
    try {
        int width = 1920, height = 1080;
        double scale = 1.0, sigma_rpx = 7.0, offset_rpx = 4.0, opacity = 0.38, radius_rpx = 5.0;
        std::vector<Rect> panels;
        std::string out, csv, shot, heatmap;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--size") { if (std::sscanf(next().c_str(), "%dx%d", &width, &height) != 2) throw std::runtime_error("--size WxH"); }
            else if (a == "--scale") scale = std::stod(next());
            else if (a == "--sigma") sigma_rpx = std::stod(next());
            else if (a == "--offset") offset_rpx = std::stod(next());
            else if (a == "--opacity") opacity = std::stod(next());
            else if (a == "--radius") radius_rpx = std::stod(next());
            else if (a == "--panel") panels.push_back(parse_rect(next()));
            else if (a == "--out") out = next();
            else if (a == "--csv") csv = next();
            else if (a == "--shot") shot = next();
            else if (a == "--heatmap") heatmap = next();
            else throw std::runtime_error("unknown argument " + a);
        }
        if (panels.empty()) throw std::runtime_error("at least one --panel is required");
        const double sigma = sigma_rpx * scale;
        const double offset = offset_rpx * scale;
        const double radius = radius_rpx * scale;

        const auto shape = coverage(width, height, panels, radius, 0.0);
        const auto shifted = coverage(width, height, panels, radius, offset);
        const auto blurred = blur(shifted, width, height, sigma);
        Image reference;
        reference.width = width;
        reference.height = height;
        reference.rgba.assign(static_cast<std::size_t>(width) * height * 4, 255);
        std::vector<double> value(static_cast<std::size_t>(width) * height, 255.0);
        for (std::size_t i = 0; i < value.size(); ++i) {
            const double alpha = opacity * blurred[i] * (1.0 - shape[i]);
            value[i] = 255.0 * (1.0 - alpha);
            const auto v = static_cast<std::uint8_t>(std::lround(shape[i] > 0.0 ? 128.0 : value[i]));
            reference.rgba[i * 4] = reference.rgba[i * 4 + 1] = reference.rgba[i * 4 + 2] = v;
        }
        if (!out.empty()) medicon::preview::save_png(out, reference);

        // Profiles outward from the middle of each side: pixel k is at d = k + 0.5.
        std::ofstream profile;
        if (!csv.empty()) {
            profile.open(csv);
            profile << "panel,side,k,d_px,reference,expected_formula,screenshot\n";
        }
        Image screenshot;
        if (!shot.empty()) screenshot = medicon::preview::load_image(shot);
        const auto shot_value = [&](int x, int y) -> double {
            if (screenshot.rgba.empty() || x < 0 || y < 0 || x >= screenshot.width || y >= screenshot.height) return -1.0;
            return screenshot.pixel(x, y)[1];
        };
        for (std::size_t p = 0; p < panels.size(); ++p) {
            const Rect& r = panels[p];
            const int mx = static_cast<int>(std::floor(r.x + r.w * 0.5));
            const int my = static_cast<int>(std::floor(r.y + r.h * 0.5));
            const int steps = static_cast<int>(std::ceil(3.0 * sigma + std::fabs(offset))) + 2;
            for (int k = 0; k < steps; ++k) {
                const double d = k + 0.5;
                struct Sample { const char* side; int x, y; double expected; };
                const Sample samples[] = {
                    {"bottom", mx, static_cast<int>(r.y + r.h) + k, 255.0 * (1.0 - opacity * phi((offset - d) / sigma))},
                    {"top", mx, static_cast<int>(r.y) - 1 - k, 255.0 * (1.0 - opacity * phi((-offset - d) / sigma))},
                    {"left", static_cast<int>(r.x) - 1 - k, my, 255.0 * (1.0 - opacity * phi(-d / sigma))},
                    {"right", static_cast<int>(r.x + r.w) + k, my, 255.0 * (1.0 - opacity * phi(-d / sigma))},
                };
                for (const auto& s : samples) {
                    if (s.x < 0 || s.y < 0 || s.x >= width || s.y >= height) continue;
                    const double ref = value[static_cast<std::size_t>(s.y) * width + s.x];
                    if (profile) {
                        profile << p << ',' << s.side << ',' << k << ',' << d << ',' << ref << ',' << s.expected << ',' << shot_value(s.x, s.y) << '\n';
                    }
                }
            }
        }

        if (!screenshot.rgba.empty()) {
            if (screenshot.width != width || screenshot.height != height) throw std::runtime_error("screenshot size differs from --size");
            Image map;
            map.width = width;
            map.height = height;
            map.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
            for (std::size_t i = 3; i < map.rgba.size(); i += 4) map.rgba[i] = 255;
            for (std::size_t p = 0; p < panels.size(); ++p) {
                const Rect& r = panels[p];
                const double ext = 3.0 * sigma + std::fabs(offset) + 2.0;
                const int x0 = std::max(0, static_cast<int>(std::floor(r.x - ext)));
                const int x1 = std::min(width, static_cast<int>(std::ceil(r.x + r.w + ext)));
                const int y0 = std::max(0, static_cast<int>(std::floor(r.y - ext)));
                const int y1 = std::min(height, static_cast<int>(std::ceil(r.y + r.h + ext)));
                double max_error = 0.0, sum = 0.0;
                std::size_t count = 0;
                int worst_x = 0, worst_y = 0;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * width + x;
                        if (shape[i] > 0.0) continue; // panel interior / its anti-aliased edge
                        double error = 0.0;
                        for (int c = 0; c < 3; ++c) error = std::max(error, std::fabs(screenshot.pixel(x, y)[c] - std::round(value[i])));
                        sum += error * error;
                        ++count;
                        if (error > max_error) { max_error = error; worst_x = x; worst_y = y; }
                        const auto heat = static_cast<std::uint8_t>(std::min(255.0, error * 40.0));
                        auto* out_pixel = map.pixel(x, y);
                        out_pixel[0] = heat;
                        out_pixel[1] = static_cast<std::uint8_t>(heat / 3);
                        out_pixel[2] = 0;
                    }
                }
                std::printf("panel %zu (%.0fx%.0f): max_error=%.0f at (%d,%d) rmse=%.3f pixels=%zu\n", p, r.w, r.h, max_error, worst_x,
                            worst_y, count ? std::sqrt(sum / static_cast<double>(count)) : 0.0, count);
            }
            if (!heatmap.empty()) medicon::preview::save_png(heatmap, map);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shadow_reference: " << error.what() << '\n';
        return 1;
    }
}
