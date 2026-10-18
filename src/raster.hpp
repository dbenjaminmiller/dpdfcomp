#pragma once
#include "image_geometry.hpp"
#include <optional>
#include <string>

namespace dpdfcomp
{
struct Raster {
    int width, height, colors, bits;
    std::string bytes;
    std::optional<ImageBounds> bounds;
    std::size_t row_bytes() const { return (std::size_t(width) * colors * bits + 7) / 8; }
    bool bilevel() const { return colors == 1 && bits == 1; }
};

unsigned sample(const Raster&, int x, int y, int channel);
ImageBounds foreground(const Raster&);
ImageBounds white_bounds(const Raster&, const std::string& pixel);
Raster crop(const Raster&, ImageBounds);
} // namespace dpdfcomp
