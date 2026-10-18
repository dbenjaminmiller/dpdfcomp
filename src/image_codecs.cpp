#include "image_codecs.hpp"
#include "compression.hpp"
#include "jbig2.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <leptonica/allheaders.h>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tiffio.hxx>

namespace dpdfcomp
{
namespace
{
using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
void configure_tiff(TIFF* tiff, const Raster& raster)
{
    if (!TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, raster.width) ||
        !TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, raster.height) ||
        !TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 1) ||
        !TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1) ||
        !TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, raster.height) ||
        !TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4) ||
        !TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE) ||
        !TIFFSetField(tiff, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB))
        throw std::runtime_error("cannot configure TIFF codec");
}

int paeth(int a, int b, int c)
{
    int p = a + b - c, da = std::abs(p - a), db = std::abs(p - b), dc = std::abs(p - c);
    return da <= db && da <= dc ? a : db <= dc ? b : c;
}

std::string png_rows(const Raster& raster, int fixed_filter)
{
    auto stride = raster.row_bytes();
    auto pixel = std::max(1, (raster.bits * raster.colors + 7) / 8);
    std::string output;
    output.reserve((stride + 1) * raster.height);
    std::array<std::string, 5> rows;
    for (auto& row : rows)
        row.resize(stride);
    for (int y = 0; y < raster.height; ++y) {
        std::array<std::uint64_t, 5> scores{};
        for (std::size_t x = 0; x < stride; ++x) {
            auto at = [&](int row, std::size_t column) {
                return static_cast<unsigned char>(raster.bytes[std::size_t(row) * stride + column]);
            };
            int current = at(y, x), a = x >= std::size_t(pixel) ? at(y, x - pixel) : 0;
            int b = y ? at(y - 1, x) : 0;
            int c = y && x >= std::size_t(pixel) ? at(y - 1, x - pixel) : 0;
            std::array<int, 5> predictions{0, a, b, (a + b) / 2, paeth(a, b, c)};
            for (int f = fixed_filter < 0 ? 0 : fixed_filter;
                 f < (fixed_filter < 0 ? 5 : fixed_filter + 1); ++f) {
                auto residual = static_cast<unsigned char>(current - predictions[f]);
                rows[f][x] = static_cast<char>(residual);
                scores[f] += std::min(int(residual), 256 - int(residual));
            }
        }
        int filter = fixed_filter < 0
                         ? int(std::min_element(scores.begin(), scores.end()) - scores.begin())
                         : fixed_filter;
        output.push_back(static_cast<char>(filter));
        output += rows[filter];
    }
    return output;
}

std::string ccitt(const Raster& raster)
{
    std::ostringstream container(std::ios::binary);
    {
        Tiff tiff(TIFFStreamOpen("dpdfcomp", static_cast<std::ostream*>(&container)), TIFFClose);
        if (!tiff)
            throw std::runtime_error("cannot initialize TIFF encoder");
        configure_tiff(tiff.get(), raster);
        if (TIFFWriteEncodedStrip(tiff.get(), 0, const_cast<char*>(raster.bytes.data()),
                                  static_cast<tmsize_t>(raster.bytes.size())) < 0)
            throw std::runtime_error("CCITT encoding failed");
    }
    std::istringstream input(container.str(), std::ios::binary);
    Tiff tiff(TIFFStreamOpen("dpdfcomp", static_cast<std::istream*>(&input)), TIFFClose);
    if (!tiff)
        throw std::runtime_error("cannot reopen TIFF data");
    auto length = TIFFRawStripSize(tiff.get(), 0);
    if (length <= 0)
        throw std::runtime_error("empty CCITT strip");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (TIFFReadRawStrip(tiff.get(), 0, result.data(), length) != length)
        throw std::runtime_error("cannot extract CCITT data");
    return result;
}

std::string jbig2(const Raster& raster, int stripe_rows)
{
    auto destroy_pix = [](PIX* pix) { pixDestroy(&pix); };
    std::unique_ptr<PIX, decltype(destroy_pix)> pix(pixCreate(raster.width, raster.height, 1),
                                                    destroy_pix);
    if (!pix)
        throw std::runtime_error("cannot allocate JBIG2 image");
    auto* data = pixGetData(pix.get());
    auto words = pixGetWpl(pix.get());
    for (int y = 0; y < raster.height; ++y) {
        auto* row = data + std::size_t(y) * words;
        for (int x = 0; x < raster.width; ++x) {
            auto byte = static_cast<unsigned char>(
                raster.bytes[std::size_t(y) * raster.row_bytes() + x / 8]);
            // PDF samples are 0=black; Leptonica/JBIG2 bitmap bits are 1=black.
            if (!(byte & (0x80 >> (x % 8))))
                SET_DATA_BIT(row, x);
        }
    }
    return encode_jbig2(data, raster.width, raster.height, stripe_rows, raster.bounds);
}

Encoding tiff_encode(const Raster& raster, bool lzw)
{
    std::ostringstream container(std::ios::binary);
    // Horizontal TIFF prediction is byte-exact for 8-bit samples. Other PDF
    // precisions use an unpredicted byte strip, avoiding TIFF endian conversion.
    const bool predicted = raster.bits == 8;
    const auto width = predicted ? std::size_t(raster.width) : raster.row_bytes();
    if (width > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("TIFF row is too wide");
    {
        Tiff tiff(TIFFStreamOpen("dpdfcomp", static_cast<std::ostream*>(&container)), TIFFClose);
        if (!tiff)
            throw std::runtime_error("cannot initialize TIFF encoder");
        if (!TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(width)) ||
            !TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, raster.height) ||
            !TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, predicted ? raster.colors : 1) ||
            !TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 8) ||
            !TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, raster.height) ||
            !TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
            !TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK) ||
            !TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION,
                          lzw ? COMPRESSION_LZW : COMPRESSION_ADOBE_DEFLATE) ||
            !TIFFSetField(tiff.get(), TIFFTAG_PREDICTOR,
                          predicted ? PREDICTOR_HORIZONTAL : PREDICTOR_NONE))
            throw std::runtime_error("cannot configure TIFF compression");
        if (!lzw)
            TIFFSetField(tiff.get(), TIFFTAG_ZIPQUALITY, 9);
        if (predicted && raster.colors > 1) {
            std::vector<std::uint16_t> extras(raster.colors - 1, EXTRASAMPLE_UNSPECIFIED);
            if (!TIFFSetField(tiff.get(), TIFFTAG_EXTRASAMPLES, static_cast<int>(extras.size()),
                              extras.data()))
                throw std::runtime_error("cannot configure TIFF sample channels");
        }
        // The predictor is permitted to modify its input buffer.
        auto pixels = raster.bytes;
        if (TIFFWriteEncodedStrip(tiff.get(), 0, pixels.data(),
                                  static_cast<tmsize_t>(pixels.size())) < 0)
            throw std::runtime_error("TIFF encoding failed");
    }
    std::istringstream input(container.str(), std::ios::binary);
    Tiff tiff(TIFFStreamOpen("dpdfcomp", static_cast<std::istream*>(&input)), TIFFClose);
    if (!tiff)
        throw std::runtime_error("cannot reopen TIFF data");
    auto length = TIFFRawStripSize(tiff.get(), 0);
    if (length <= 0)
        throw std::runtime_error("empty TIFF strip");
    std::string bytes(static_cast<std::size_t>(length), '\0');
    if (TIFFReadRawStrip(tiff.get(), 0, bytes.data(), length) != length)
        throw std::runtime_error("cannot extract TIFF strip");
    return {lzw ? Codec::lzw : Codec::zip, std::move(bytes), predicted ? 2 : 1};
}
} // namespace

std::vector<Encoding> encode_bilevel(const Raster& raster, const Options& options)
{
    std::vector<Encoding> result;
    if (options.bilevel_flate) {
        result.push_back({Codec::flate, deflate(raster.bytes)});
        std::string best;
        for (int filter = -1; filter < 5; ++filter) {
            auto candidate = deflate(png_rows(raster, filter));
            if (best.empty() || candidate.size() < best.size())
                best = std::move(candidate);
        }
        result.push_back({Codec::png, std::move(best), 15});
    }
    if (options.bilevel_png_iterations)
        result.push_back({Codec::png, png_optimize(raster, options.bilevel_png_iterations), 15});
    if (options.ccitt)
        result.push_back({Codec::ccitt, ccitt(raster)});
    if (options.jbig2)
        result.push_back({Codec::jbig2, jbig2(raster, options.jbig2_stripe_rows)});
    return result;
}

std::vector<Encoding> encode_color(const Raster& raster, const std::vector<ColorEncoding>& codecs,
                                   bool device_color)
{
    std::vector<Encoding> result;
    for (auto encoding : codecs) {
        switch (encoding.codec) {
        case ColorCodec::png:
            if (raster.colors <= 4 && (raster.colors == 1 || raster.bits >= 8))
                result.push_back({Codec::png, png_optimize(raster, encoding.parameter), 15});
            break;
        case ColorCodec::lzw:
        case ColorCodec::zip:
            result.push_back(tiff_encode(raster, encoding.codec == ColorCodec::lzw));
            break;
        case ColorCodec::jpeg:
            if (device_color && raster.bits == 8 && (raster.colors == 1 || raster.colors == 3))
                result.push_back({Codec::jpeg, encode_jpeg(raster, encoding.parameter), 1, true});
            break;
        case ColorCodec::jp2_lossless:
        case ColorCodec::jp2_lossy:
            if (encoding.codec == ColorCodec::jp2_lossless || device_color) {
                if (raster.bits >= 8)
                    result.push_back({Codec::jp2,
                                      encode_jp2(raster, encoding.codec == ColorCodec::jp2_lossless,
                                                 encoding.parameter),
                                      1, encoding.codec == ColorCodec::jp2_lossy});
                else if (device_color) {
                    // Some viewers left-shift sub-byte JPX samples rather than
                    // expanding their full range. Exact 8-bit endpoint expansion
                    // avoids darkening while preserving gray/RGB values.
                    Raster expanded{raster.width, raster.height, raster.colors, 8, {}, {}};
                    expanded.bytes.reserve(expanded.row_bytes() * expanded.height);
                    const auto factor = 255U / ((1U << raster.bits) - 1);
                    for (int y = 0; y < raster.height; ++y)
                        for (int x = 0; x < raster.width; ++x)
                            for (int c = 0; c < raster.colors; ++c)
                                expanded.bytes +=
                                    static_cast<char>(sample(raster, x, y, c) * factor);
                    result.push_back(
                        {Codec::jp2,
                         encode_jp2(expanded, encoding.codec == ColorCodec::jp2_lossless,
                                    encoding.parameter),
                         1, encoding.codec == ColorCodec::jp2_lossy, 8});
                }
            }
            break;
        case ColorCodec::bilevel:
            break; // Pixel conversion happens before cropping and encoding.
        }
    }
    return result;
}
} // namespace dpdfcomp
