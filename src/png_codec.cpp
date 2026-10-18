#include "compression.hpp"
#include "image_codecs.hpp"
#include <zlib.h>
#include <zopflipng_lib.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace dpdfcomp
{
namespace
{
void append32(std::string& out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>(value >> shift));
}

std::uint32_t read32(const unsigned char* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
}

void chunk(std::string& out, std::string_view type, std::string_view bytes)
{
    if (bytes.size() > 0x7fffffff)
        throw std::runtime_error("PNG chunk is too large");
    append32(out, static_cast<std::uint32_t>(bytes.size()));
    const auto start = out.size();
    out.append(type);
    out.append(bytes);
    append32(out, crc32(0, reinterpret_cast<const Bytef*>(out.data() + start),
                        static_cast<uInt>(bytes.size() + 4)));
}
} // namespace

std::string png_optimize(const Raster& raster, int iterations)
{
    // PNG is a container for the sample layout here. In particular, four
    // channels may be CMYK: lossless optimization must preserve every channel,
    // including values that PNG would interpret as fully transparent pixels.
    const std::array<unsigned char, 5> types{0, 0, 4, 2, 6};
    if (raster.colors < 1 || raster.colors > 4 || (raster.colors != 1 && raster.bits < 8))
        throw std::runtime_error("PNG does not support this sample layout");
    std::string header;
    append32(header, raster.width);
    append32(header, raster.height);
    header += static_cast<char>(raster.bits);
    header += static_cast<char>(types[raster.colors]);
    header.append(3, '\0');
    std::string rows;
    rows.reserve((raster.row_bytes() + 1) * raster.height);
    for (int y = 0; y < raster.height; ++y) {
        rows += '\0';
        rows.append(raster.bytes, std::size_t(y) * raster.row_bytes(), raster.row_bytes());
    }
    std::string input("\x89PNG\r\n\x1a\n", 8);
    chunk(input, "IHDR", header);
    chunk(input, "IDAT", deflate(rows));
    chunk(input, "IEND", {});
    ZopfliPNGOptions options;
    options.keep_colortype = true;
    options.lossy_transparent = false;
    options.lossy_8bit = false;
    options.num_iterations = options.num_iterations_large = iterations;
    options.auto_filter_strategy = false;
    for (int strategy = 0; strategy < kNumFilterStrategies; ++strategy)
        options.filter_strategies.push_back(static_cast<ZopfliPNGFilterStrategy>(strategy));
    std::vector<unsigned char> optimized;
    const auto error = ZopfliPNGOptimize({input.begin(), input.end()}, options, false, &optimized);
    if (error)
        throw std::runtime_error("ZopfliPNG failed (" + std::to_string(error) + ")");
    std::string result;
    bool checked_header = false;
    for (std::size_t pos = 8; pos + 12 <= optimized.size();) {
        const auto length = read32(optimized.data() + pos);
        if (length > optimized.size() - pos - 12)
            throw std::runtime_error("invalid optimized PNG chunk");
        const std::string_view type(reinterpret_cast<const char*>(optimized.data() + pos + 4), 4);
        const std::string_view data(reinterpret_cast<const char*>(optimized.data() + pos + 8),
                                    length);
        if (type == "IHDR") {
            if (data != header)
                throw std::runtime_error("PNG optimizer changed the sample layout");
            checked_header = true;
        } else if (type == "IDAT")
            result.append(data);
        pos += std::size_t(length) + 12;
    }
    if (!checked_header || result.empty())
        throw std::runtime_error("PNG optimizer produced no image data");
    return result;
}
} // namespace dpdfcomp
