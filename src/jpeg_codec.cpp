#include "image_codecs.hpp"
#include <limits>
#include <memory>
#include <stdexcept>
#include <turbojpeg.h>

namespace dpdfcomp
{
std::string encode_jpeg(const Raster& raster, int quality)
{
    using Compressor = std::unique_ptr<void, decltype(&tjDestroy)>;
    Compressor compressor(tjInitCompress(), tjDestroy);
    if (!compressor)
        throw std::runtime_error("cannot initialize JPEG encoder");
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    const auto status = tjCompress2(
        compressor.get(), reinterpret_cast<const unsigned char*>(raster.bytes.data()), raster.width,
        0, raster.height, raster.colors == 1 ? TJPF_GRAY : TJPF_RGB, &buffer, &size,
        raster.colors == 1 ? TJSAMP_GRAY : TJSAMP_444, quality, TJFLAG_ACCURATEDCT);
    std::unique_ptr<unsigned char, decltype(&tjFree)> owned(buffer, tjFree);
    if (status < 0)
        throw std::runtime_error(std::string("JPEG encoding failed: ") +
                                 tjGetErrorStr2(compressor.get()));
    return {reinterpret_cast<const char*>(buffer), size};
}
} // namespace dpdfcomp

namespace dpdfcomp
{
Raster decode_jpeg(const std::string& bytes, std::optional<int> color_transform)
{
    using Decompressor = std::unique_ptr<void, decltype(&tjDestroy)>;
    Decompressor decoder(tjInitDecompress(), tjDestroy);
    if (!decoder)
        throw std::runtime_error("cannot initialize JPEG decoder");
    int width, height, subsampling, space;
    const auto* input = reinterpret_cast<const unsigned char*>(bytes.data());
    if (tjDecompressHeader3(decoder.get(), input, bytes.size(), &width, &height, &subsampling,
                            &space) < 0)
        throw std::runtime_error("invalid JPEG header");
    const bool transformed = space == TJCS_YCbCr || space == TJCS_YCCK;
    if (color_transform && space != TJCS_GRAY && bool(*color_transform) != transformed)
        throw std::runtime_error("JPEG with an overriding PDF ColorTransform is preserved");
    const int colors = space == TJCS_GRAY ? 1 : space == TJCS_CMYK || space == TJCS_YCCK ? 4 : 3;
    Raster raster{width, height, colors, 8, {}, {}};
    if (width <= 0 || height <= 0 ||
        raster.row_bytes() > std::numeric_limits<std::size_t>::max() / height)
        throw std::runtime_error("invalid JPEG dimensions");
    raster.bytes.resize(raster.row_bytes() * height);
    if (tjDecompress2(decoder.get(), input, bytes.size(),
                      reinterpret_cast<unsigned char*>(raster.bytes.data()), width, 0, height,
                      colors == 1   ? TJPF_GRAY
                      : colors == 4 ? TJPF_CMYK
                                    : TJPF_RGB,
                      TJFLAG_ACCURATEDCT | TJFLAG_STOPONWARNING) < 0)
        throw std::runtime_error(std::string("JPEG decoding failed: ") +
                                 tjGetErrorStr2(decoder.get()));
    return raster;
}
} // namespace dpdfcomp
