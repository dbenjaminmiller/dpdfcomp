#pragma once
#include "raster.hpp"
#include <dpdfcomp/optimizer.hpp>
#include <vector>

namespace dpdfcomp
{
enum class Codec { flate, png, lzw, zip, jpeg, jp2, ccitt, jbig2 };
struct Encoding {
    Codec codec;
    std::string bytes;
    int predictor = 1;
    bool lossy = false;
    int bits = 0; // Nonzero when exact sample expansion changes the stored precision.
};

std::vector<Encoding> encode_bilevel(const Raster&, const Options&);
std::vector<Encoding> encode_color(const Raster&, const std::vector<ColorEncoding>&,
                                   bool device_color);
std::string decode_jbig2(const std::string& bytes, const std::string& globals, int width,
                         int height);
Raster decode_jpeg(const std::string& bytes, std::optional<int> color_transform);
Raster decode_jp2(const std::string& bytes);
std::string encode_jpeg(const Raster&, int quality);
std::string encode_jp2(const Raster&, bool lossless, int quality);
std::string png_optimize(const Raster&, int iterations);
} // namespace dpdfcomp
