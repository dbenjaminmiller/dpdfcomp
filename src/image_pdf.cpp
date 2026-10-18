#include "image_pdf.hpp"
#include "image_codecs.hpp"
#include <algorithm>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tiffio.hxx>

namespace dpdfcomp
{
using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

std::optional<std::string> white_pixel(Object source, const Raster& raster)
{
    auto dictionary = source.getDict();
    auto space = dictionary.getKey("/ColorSpace");
    bool cmyk = space.isNameAndEquals("/DeviceCMYK");
    if (!cmyk && !space.isNameAndEquals("/DeviceGray") && !space.isNameAndEquals("/DeviceRGB"))
        return std::nullopt;
    auto decode = dictionary.getKey("/Decode");
    if (!decode.isNull() && (!decode.isArray() || decode.getArrayNItems() != 2 * raster.colors))
        return std::nullopt;
    std::string bytes((raster.colors * raster.bits + 7) / 8, '\0');
    for (int channel = 0; channel < raster.colors; ++channel) {
        bool high = !cmyk;
        if (!decode.isNull()) {
            auto low = decode.getArrayItem(2 * channel),
                 upper = decode.getArrayItem(2 * channel + 1);
            if (!low.isNumber() || !upper.isNumber())
                return std::nullopt;
            if (low.getNumericValue() == 1 && upper.getNumericValue() == 0)
                high = !high;
            else if (low.getNumericValue() != 0 || upper.getNumericValue() != 1)
                return std::nullopt;
        }
        if (high)
            for (int bit = channel * raster.bits; bit < (channel + 1) * raster.bits; ++bit)
                bytes[bit / 8] |= static_cast<char>(128 >> (bit % 8));
    }
    return bytes;
}

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

static std::string decode_ccitt(Object source, const Raster& raster)
{
    auto params = source.getDict().getKey("/DecodeParms");
    if (params.isArray() && params.getArrayNItems() == 1)
        params = params.getArrayItem(0);
    if (!params.isDictionary())
        throw std::runtime_error("unsupported CCITT parameters");
    auto value = [&](const char* key, long long fallback) {
        auto v = params.getKey(key);
        if (v.isNull())
            return fallback;
        if (!v.isInteger())
            throw std::runtime_error("invalid CCITT parameter");
        return v.getIntValue();
    };
    auto flag = [&](const char* key) {
        auto v = params.getKey(key);
        if (v.isNull())
            return false;
        if (!v.isBool())
            throw std::runtime_error("invalid CCITT parameter");
        return v.getBoolValue();
    };
    auto rows = value("/Rows", 0);
    if (!raster.bilevel() || value("/K", 0) >= 0 || value("/Columns", 1728) != raster.width ||
        (rows != 0 && rows != raster.height) || flag("/EncodedByteAlign") ||
        value("/DamagedRowsBeforeError", 0) != 0)
        throw std::runtime_error("only ordinary CCITT Group 4 input is supported");
    auto raw = raw_bytes(source);
    std::ostringstream container(std::ios::binary);
    {
        Tiff tiff(TIFFStreamOpen("dpdfcomp", static_cast<std::ostream*>(&container)), TIFFClose);
        if (!tiff)
            throw std::runtime_error("cannot initialize TIFF wrapper");
        configure_tiff(tiff.get(), raster);
        if (TIFFWriteRawStrip(tiff.get(), 0, raw.data(), static_cast<tmsize_t>(raw.size())) < 0)
            throw std::runtime_error("cannot wrap CCITT input");
    }
    std::istringstream input(container.str(), std::ios::binary);
    Tiff tiff(TIFFStreamOpen("dpdfcomp", static_cast<std::istream*>(&input)), TIFFClose);
    if (!tiff)
        throw std::runtime_error("cannot reopen CCITT input");
    std::string decoded(raster.row_bytes() * raster.height, '\0');
    if (TIFFReadEncodedStrip(tiff.get(), 0, decoded.data(),
                             static_cast<tmsize_t>(decoded.size())) !=
        static_cast<tmsize_t>(decoded.size()))
        throw std::runtime_error("CCITT decoding failed");
    if (!flag("/BlackIs1"))
        for (auto& byte : decoded)
            byte = static_cast<char>(~byte);
    return decoded;
}

static int integer(Object dictionary, const std::string& key)
{
    auto value = dictionary.getKey(key);
    if (!value.isInteger() || value.getIntValue() <= 0 ||
        value.getIntValue() > std::numeric_limits<int>::max())
        throw std::runtime_error("invalid " + key);
    return static_cast<int>(value.getIntValue());
}

int components(Object space)
{
    if (space.isNameAndEquals("/DeviceGray"))
        return 1;
    if (space.isNameAndEquals("/DeviceRGB"))
        return 3;
    if (space.isNameAndEquals("/DeviceCMYK"))
        return 4;
    if (!space.isArray() || space.getArrayNItems() < 2)
        return 0;
    auto kind = space.getArrayItem(0);
    if (kind.isNameAndEquals("/Indexed") || kind.isNameAndEquals("/Separation") ||
        kind.isNameAndEquals("/CalGray"))
        return 1;
    if (kind.isNameAndEquals("/CalRGB") || kind.isNameAndEquals("/Lab"))
        return 3;
    if (kind.isNameAndEquals("/ICCBased") && space.getArrayItem(1).isStream()) {
        int count = integer(space.getArrayItem(1).getDict(), "/N");
        return count == 1 || count == 3 || count == 4 ? count : 0;
    }
    if (kind.isNameAndEquals("/DeviceN") && space.getArrayItem(1).isArray())
        return space.getArrayItem(1).getArrayNItems();
    return 0; // Resource names require a resource context; leave them alone.
}

static bool generalized_filter(Object filter)
{
    if (filter.isNull())
        return true;
    if (filter.isArray()) {
        for (auto item : filter.getArrayAsVector())
            if (!generalized_filter(item))
                return false;
        return true;
    }
    for (const auto* name :
         {"/FlateDecode", "/LZWDecode", "/RunLengthDecode", "/ASCII85Decode", "/ASCIIHexDecode"})
        if (filter.isNameAndEquals(name))
            return true;
    return false;
}

Raster read_image(Object source)
{
    auto d = source.getDict();
    if (d.hasKey("/F") || d.hasKey("/Alternates"))
        throw std::runtime_error("external/alternate image data is preserved");
    bool mask = d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue();
    auto initial_filter = d.getKey("/Filter");
    if (initial_filter.isArray() && initial_filter.getArrayNItems() == 1)
        initial_filter = initial_filter.getArrayItem(0);
    std::optional<Raster> jpx;
    if (initial_filter.isNameAndEquals("/JPXDecode") && !d.hasKey("/BitsPerComponent"))
        jpx = decode_jp2(raw_bytes(source));
    Raster raster{integer(d, "/Width"),
                  integer(d, "/Height"),
                  mask ? 1 : components(d.getKey("/ColorSpace")),
                  mask  ? 1
                  : jpx ? jpx->bits
                        : integer(d, "/BitsPerComponent"),
                  {},
                  {}};
    if (raster.colors <= 0 || raster.colors > 32 ||
        (raster.bits != 1 && raster.bits != 2 && raster.bits != 4 && raster.bits != 8 &&
         raster.bits != 16))
        throw std::runtime_error("unsupported image sample layout");
    if (raster.row_bytes() > std::numeric_limits<std::size_t>::max() / raster.height)
        throw std::runtime_error("image dimensions overflow");
    auto filter = d.getKey("/Filter");
    if (filter.isArray() && filter.getArrayNItems() == 1)
        filter = filter.getArrayItem(0);
    if (filter.isNameAndEquals("/CCITTFaxDecode"))
        raster.bytes = decode_ccitt(source, raster);
    else if (filter.isNameAndEquals("/JBIG2Decode")) {
        auto parameters = d.getKey("/DecodeParms");
        if (parameters.isArray() && parameters.getArrayNItems() == 1)
            parameters = parameters.getArrayItem(0);
        auto globals =
            parameters.isDictionary() ? parameters.getKey("/JBIG2Globals") : Object::newNull();
        if (!raster.bilevel() || (!globals.isNull() && !globals.isStream()))
            throw std::runtime_error("invalid JBIG2 image layout or globals");
        raster.bytes =
            decode_jbig2(raw_bytes(source), globals.isStream() ? decoded_bytes(globals) : "",
                         raster.width, raster.height);
    } else if (generalized_filter(filter))
        raster.bytes = decoded_bytes(source);
    else if (filter.isNameAndEquals("/DCTDecode")) {
        auto parameters = d.getKey("/DecodeParms");
        if (parameters.isArray() && parameters.getArrayNItems() == 1)
            parameters = parameters.getArrayItem(0);
        auto transform =
            parameters.isDictionary() ? parameters.getKey("/ColorTransform") : Object::newNull();
        std::optional<int> color_transform;
        if (!transform.isNull()) {
            if (!transform.isInteger() ||
                (transform.getIntValue() != 0 && transform.getIntValue() != 1))
                throw std::runtime_error("invalid JPEG ColorTransform");
            color_transform = static_cast<int>(transform.getIntValue());
        }
        auto decoded = decode_jpeg(raw_bytes(source), color_transform);
        if (decoded.width != raster.width || decoded.height != raster.height ||
            decoded.colors != raster.colors || decoded.bits != raster.bits)
            throw std::runtime_error("JPEG samples do not match the PDF dictionary");
        raster.bytes = std::move(decoded.bytes);
    } else if (filter.isNameAndEquals("/JPXDecode")) {
        auto alpha = d.getKey("/SMaskInData");
        if ((!alpha.isNull() && (!alpha.isInteger() || alpha.getIntValue() != 0)) ||
            !d.getKey("/Decode").isNull())
            throw std::runtime_error("JPEG 2000 alpha/Decode attributes are preserved");
        auto decoded = jpx ? std::move(*jpx) : decode_jp2(raw_bytes(source));
        if (decoded.width != raster.width || decoded.height != raster.height ||
            decoded.colors != raster.colors || decoded.bits != raster.bits)
            throw std::runtime_error("JPEG 2000 samples do not match the PDF dictionary");
        raster.bytes = std::move(decoded.bytes);
    } else
        throw std::runtime_error("existing image codec is preserved");
    if (raster.bytes.size() != raster.row_bytes() * raster.height)
        throw std::runtime_error("decoded image length does not match its dimensions");
    return raster;
}

bool lossy_input(Object filter)
{
    if (filter.isArray()) {
        for (auto item : filter.getArrayAsVector())
            if (lossy_input(item))
                return true;
    }
    // A reversible JPEG 2000 transform alone does not establish losslessness:
    // an encoder may have truncated quality layers. Treat unknown provenance
    // conservatively and require the color-lossy option for re-encoding JPX.
    return filter.isNameAndEquals("/DCTDecode") || filter.isNameAndEquals("/JPXDecode");
}

bool matches_codec(Object source, const Options& options, bool bilevel,
                   const std::vector<ColorEncoding>& color)
{
    auto filter = source.getDict().getKey("/Filter");
    auto parameters = source.getDict().getKey("/DecodeParms");
    if (filter.isArray()) {
        if (!filter.getArrayNItems())
            return false;
        // ASCII wrappers do not change the underlying image codec.
        filter = filter.getArrayItem(filter.getArrayNItems() - 1);
        if (parameters.isArray() && parameters.getArrayNItems())
            parameters = parameters.getArrayItem(parameters.getArrayNItems() - 1);
    }
    const auto predictor =
        parameters.isDictionary() ? parameters.getKey("/Predictor") : Object::newNull();
    const bool png =
        predictor.isInteger() && predictor.getIntValue() >= 10 && predictor.getIntValue() <= 15;
    if (bilevel) {
        return (filter.isNameAndEquals("/FlateDecode") &&
                (options.bilevel_flate || (png && options.bilevel_png_iterations))) ||
               (filter.isNameAndEquals("/CCITTFaxDecode") && options.ccitt) ||
               (filter.isNameAndEquals("/JBIG2Decode") && options.jbig2);
    }
    return std::any_of(color.begin(), color.end(), [&](auto encoding) {
        switch (encoding.codec) {
        case ColorCodec::png:
            return png && filter.isNameAndEquals("/FlateDecode");
        case ColorCodec::zip:
            return !png && filter.isNameAndEquals("/FlateDecode");
        case ColorCodec::lzw:
            return filter.isNameAndEquals("/LZWDecode");
        case ColorCodec::jpeg:
            return filter.isNameAndEquals("/DCTDecode");
        case ColorCodec::jp2_lossless:
        case ColorCodec::jp2_lossy:
            return filter.isNameAndEquals("/JPXDecode");
        case ColorCodec::bilevel:
            return false;
        }
        return false;
    });
}

BilevelInput bilevel_input(Object source, const Raster& raster)
{
    BilevelInput result;
    auto d = source.getDict();
    auto space = d.getKey("/ColorSpace");
    if (!space.isNameAndEquals("/DeviceGray") && !space.isNameAndEquals("/DeviceRGB"))
        return result;
    auto decode = d.getKey("/Decode");
    if (!decode.isNull()) {
        if (!decode.isArray() || decode.getArrayNItems() != 2 * raster.colors)
            return result;
        for (int c = 0; c < raster.colors; ++c) {
            auto low = decode.getArrayItem(2 * c), high = decode.getArrayItem(2 * c + 1);
            if (!low.isNumber() || !high.isNumber())
                return result;
            if (low.getNumericValue() == 1 && high.getNumericValue() == 0)
                result.reversed[c] = true;
            else if (low.getNumericValue() != 0 || high.getNumericValue() != 1)
                return result;
        }
    }
    auto mask = d.getKey("/Mask"), alpha = d.getKey("/SMask");
    if (mask.isArray()) {
        if (mask.getArrayNItems() != 2 * raster.colors || !alpha.isNull())
            return result;
        for (auto item : mask.getArrayAsVector()) {
            if (!item.isInteger() || item.getIntValue() < 0 ||
                item.getIntValue() >= (1LL << raster.bits))
                return result;
            result.color_key.push_back(static_cast<unsigned>(item.getIntValue()));
        }
    }
    if (alpha.isStream()) {
        auto ad = alpha.getDict();
        if (ad.hasKey("/Matte"))
            return result; // Matte values belong to the original component space.
        if (ad.getKey("/ColorSpace").isNameAndEquals("/DeviceGray")) {
            auto a_decode = ad.getKey("/Decode");
            bool ordinary = a_decode.isNull() || a_decode.unparse() == "[ 0 1 ]";
            result.alpha_reversed = a_decode.unparse() == "[ 1 0 ]";
            if (ordinary || result.alpha_reversed) {
                try {
                    auto samples = read_image(alpha);
                    if (samples.width == raster.width && samples.height == raster.height)
                        result.alpha = std::move(samples);
                } catch (const std::exception&) {
                    // Exact black/white conversion is still safe when keeping
                    // an opaque-to-us soft mask unchanged. Do not ignore pixels.
                }
            }
        }
    }
    result.colors = raster.colors;
    return result;
}

} // namespace dpdfcomp
