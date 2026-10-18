#include "raster.hpp"
#include <algorithm>

namespace dpdfcomp
{
ImageBounds foreground(const Raster& raster)
{
    int left = raster.width, top = raster.height, right = 0, bottom = 0;
    for (int y = 0; y < raster.height; ++y)
        for (std::size_t byte = 0; byte < raster.row_bytes(); ++byte) {
            const auto bits = static_cast<unsigned char>(
                raster.bytes[std::size_t(y) * raster.row_bytes() + byte]);
            if (bits == 255)
                continue;
            for (int bit = 0; bit < 8 && byte * 8 + bit < std::size_t(raster.width); ++bit)
                if (!(bits & (128 >> bit))) {
                    const auto x = int(byte * 8 + bit);
                    left = std::min(left, x);
                    top = std::min(top, y);
                    right = std::max(right, x + 1);
                    bottom = y + 1;
                }
        }
    return right ? ImageBounds{left, top, right - left, bottom - top} : ImageBounds{};
}

unsigned sample(const Raster& raster, int x, int y, int channel)
{
    const auto bit = (std::size_t(x) * raster.colors + channel) * raster.bits;
    const auto offset = std::size_t(y) * raster.row_bytes() + bit / 8;
    const auto byte = static_cast<unsigned char>(raster.bytes[offset]);
    if (raster.bits == 16)
        return (unsigned(byte) << 8) | static_cast<unsigned char>(raster.bytes[offset + 1]);
    return (byte >> (8 - raster.bits - bit % 8)) & ((1U << raster.bits) - 1);
}

ImageBounds white_bounds(const Raster& raster, const std::string& pixel)
{
    Raster white{1, 1, raster.colors, raster.bits, pixel, {}};
    int left = raster.width, top = raster.height, right = 0, bottom = 0;
    for (int y = 0; y < raster.height; ++y)
        for (int x = 0; x < raster.width; ++x)
            for (int channel = 0; channel < raster.colors; ++channel)
                if (sample(raster, x, y, channel) != sample(white, 0, 0, channel)) {
                    left = std::min(left, x);
                    right = std::max(right, x + 1);
                    top = std::min(top, y);
                    bottom = y + 1;
                    break;
                }
    return right ? ImageBounds{left, top, right - left, bottom - top} : ImageBounds{};
}

Raster crop(const Raster& source, ImageBounds area)
{
    Raster result{area.width, area.height, source.colors, source.bits, {}, {}};
    result.bytes.resize(result.row_bytes() * area.height);
    const auto first_bit = std::size_t(area.x) * source.colors * source.bits;
    const auto shift = first_bit % 8;
    for (int y = 0; y < area.height; ++y) {
        const auto offset = std::size_t(y + area.y) * source.row_bytes();
        for (std::size_t byte = 0; byte < result.row_bytes(); ++byte) {
            const auto column = first_bit / 8 + byte;
            auto bits = unsigned(static_cast<unsigned char>(source.bytes[offset + column]))
                        << shift;
            if (shift && column + 1 < source.row_bytes())
                bits |=
                    static_cast<unsigned char>(source.bytes[offset + column + 1]) >> (8 - shift);
            result.bytes[std::size_t(y) * result.row_bytes() + byte] = static_cast<char>(bits);
        }
    }
    return result;
}

} // namespace dpdfcomp
