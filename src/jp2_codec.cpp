#include "image_codecs.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <openjpeg.h>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{
using Jp2Codec = std::unique_ptr<opj_codec_t, decltype(&opj_destroy_codec)>;
using Image = std::unique_ptr<opj_image_t, decltype(&opj_image_destroy)>;
using Stream = std::unique_ptr<opj_stream_t, decltype(&opj_stream_destroy)>;

struct MemoryStream {
    std::string bytes;
    std::size_t offset = 0;
    bool failed = false;
};

// OpenJPEG calls C callbacks. Never let a C++ exception cross that boundary.
OPJ_SIZE_T read(void* out, OPJ_SIZE_T size, void* user)
{
    auto& memory = *static_cast<MemoryStream*>(user);
    size = std::min(size, memory.bytes.size() - memory.offset);
    if (!size)
        return OPJ_SIZE_T(-1);
    std::memcpy(out, memory.bytes.data() + memory.offset, size);
    memory.offset += size;
    return size;
}
OPJ_SIZE_T write(void* input, OPJ_SIZE_T size, void* user)
{
    auto& memory = *static_cast<MemoryStream*>(user);
    try {
        if (size > memory.bytes.max_size() - memory.offset)
            throw std::length_error("JPEG 2000 output overflow");
        if (memory.offset + size > memory.bytes.size())
            memory.bytes.resize(memory.offset + size);
        std::memcpy(memory.bytes.data() + memory.offset, input, size);
        memory.offset += size;
        return size;
    } catch (...) {
        memory.failed = true;
        return OPJ_SIZE_T(-1);
    }
}
OPJ_BOOL seek(OPJ_OFF_T offset, void* user)
{
    auto& memory = *static_cast<MemoryStream*>(user);
    if (offset < 0 || std::uint64_t(offset) > memory.bytes.size())
        return OPJ_FALSE;
    memory.offset = static_cast<std::size_t>(offset);
    return OPJ_TRUE;
}
OPJ_OFF_T skip(OPJ_OFF_T amount, void* user)
{
    auto& memory = *static_cast<MemoryStream*>(user);
    if (amount > 0 && std::uint64_t(amount) > memory.bytes.size() - memory.offset)
        return -1;
    if (amount < 0 && amount < -static_cast<OPJ_OFF_T>(memory.offset))
        return -1;
    memory.offset = static_cast<std::size_t>(static_cast<OPJ_OFF_T>(memory.offset) + amount);
    return amount;
}
void quiet(const char*, void*) {}

Stream stream(MemoryStream& memory, bool input)
{
    Stream result(opj_stream_default_create(input), opj_stream_destroy);
    if (!result)
        throw std::runtime_error("cannot initialize JPEG 2000 stream");
    opj_stream_set_user_data(result.get(), &memory, nullptr);
    opj_stream_set_user_data_length(result.get(), memory.bytes.size());
    opj_stream_set_read_function(result.get(), read);
    opj_stream_set_write_function(result.get(), write);
    opj_stream_set_skip_function(result.get(), skip);
    opj_stream_set_seek_function(result.get(), seek);
    return result;
}
void configure(opj_codec_t* codec)
{
    if (!codec)
        throw std::runtime_error("cannot initialize JPEG 2000 codec");
    opj_set_info_handler(codec, quiet, nullptr);
    opj_set_warning_handler(codec, quiet, nullptr);
    opj_set_error_handler(codec, quiet, nullptr);
}

Raster sample_layout(const opj_image_t& image, bool decoded)
{
    if (!image.numcomps || image.numcomps > 32 || !image.comps || image.x0 || image.y0 ||
        image.x1 > std::numeric_limits<int>::max() || image.y1 > std::numeric_limits<int>::max() ||
        image.color_space == OPJ_CLRSPC_SYCC || image.color_space == OPJ_CLRSPC_EYCC)
        throw std::runtime_error("unsupported JPEG 2000 color layout");
    Raster result{
        int(image.x1), int(image.y1), int(image.numcomps), int(image.comps[0].prec), {}, {}};
    if (!result.width || !result.height ||
        (result.bits != 1 && result.bits != 2 && result.bits != 4 && result.bits != 8 &&
         result.bits != 16) ||
        result.row_bytes() > std::numeric_limits<std::size_t>::max() / result.height)
        throw std::runtime_error("unsupported JPEG 2000 dimensions or precision");
    for (unsigned c = 0; c < image.numcomps; ++c) {
        const auto& component = image.comps[c];
        if (component.w != image.x1 || component.h != image.y1 ||
            component.prec != unsigned(result.bits) || component.sgnd || component.alpha ||
            component.dx != 1 || component.dy != 1 || component.x0 || component.y0 ||
            (decoded && !component.data))
            throw std::runtime_error("unsupported JPEG 2000 component layout");
    }
    return result;
}
} // namespace

std::string encode_jp2(const Raster& raster, bool lossless, int quality)
{
    std::vector<opj_image_cmptparm_t> components(raster.colors);
    for (auto& component : components) {
        component.dx = component.dy = 1;
        component.w = raster.width;
        component.h = raster.height;
        component.prec = raster.bits;
    }
    Image image(opj_image_create(raster.colors, components.data(), OPJ_CLRSPC_UNSPECIFIED),
                opj_image_destroy);
    if (!image)
        throw std::runtime_error("cannot allocate JPEG 2000 image");
    image->x1 = raster.width;
    image->y1 = raster.height;
    for (int c = 0; c < raster.colors; ++c)
        for (int y = 0; y < raster.height; ++y)
            for (int x = 0; x < raster.width; ++x)
                image->comps[c].data[std::size_t(y) * raster.width + x] = sample(raster, x, y, c);
    opj_cparameters_t parameters;
    opj_set_default_encoder_parameters(&parameters);
    parameters.tcp_numlayers = 1;
    parameters.numresolution = 1;
    for (auto size = std::min(raster.width, raster.height);
         size >= 2 && parameters.numresolution < 6; size /= 2)
        ++parameters.numresolution;
    parameters.irreversible = !lossless;
    if (lossless) {
        parameters.cp_disto_alloc = 1;
        parameters.tcp_rates[0] = 0;
    } else {
        parameters.cp_fixed_quality = 1;
        // A monotonic, documented PSNR target, not a claim of JPEG equivalence.
        parameters.tcp_distoratio[0] = 20.0f + 0.4f * quality;
    }
    Jp2Codec codec(opj_create_compress(OPJ_CODEC_J2K), opj_destroy_codec);
    configure(codec.get());
    MemoryStream memory;
    auto output = stream(memory, false);
    if (!opj_setup_encoder(codec.get(), &parameters, image.get()) ||
        !opj_codec_set_threads(codec.get(), 1) ||
        !opj_start_compress(codec.get(), image.get(), output.get()) ||
        !opj_encode(codec.get(), output.get()) || !opj_end_compress(codec.get(), output.get()) ||
        memory.failed)
        throw std::runtime_error("JPEG 2000 encoding failed");
    return std::move(memory.bytes);
}

Raster decode_jp2(const std::string& bytes)
{
    const bool codestream = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
                            static_cast<unsigned char>(bytes[1]) == 0x4f;
    Jp2Codec codec(opj_create_decompress(codestream ? OPJ_CODEC_J2K : OPJ_CODEC_JP2),
                   opj_destroy_codec);
    configure(codec.get());
    opj_dparameters_t parameters;
    opj_set_default_decoder_parameters(&parameters);
    MemoryStream memory{bytes};
    auto input = stream(memory, true);
    opj_image_t* raw_image = nullptr;
    if (!opj_setup_decoder(codec.get(), &parameters) || !opj_codec_set_threads(codec.get(), 1) ||
        !opj_read_header(input.get(), codec.get(), &raw_image)) {
        if (raw_image)
            opj_image_destroy(raw_image);
        throw std::runtime_error("cannot read JPEG 2000 image");
    }
    Image image(raw_image, opj_image_destroy);
    sample_layout(*image, false);
    if (!opj_decode(codec.get(), input.get(), image.get()) ||
        !opj_end_decompress(codec.get(), input.get()))
        throw std::runtime_error("JPEG 2000 decoding failed");
    // JP2 palette/channel mapping can replace the components during decoding.
    // Both their count and precision must come from the final sample layout.
    auto result = sample_layout(*image, true);
    result.bytes.resize(result.row_bytes() * result.height);
    for (int y = 0; y < result.height; ++y)
        for (int x = 0; x < result.width; ++x)
            for (int c = 0; c < result.colors; ++c) {
                const auto value = image->comps[c].data[std::size_t(y) * result.width + x];
                const auto bit = (std::size_t(x) * result.colors + c) * result.bits;
                const auto pos = std::size_t(y) * result.row_bytes() + bit / 8;
                if (result.bits == 16) {
                    result.bytes[pos] = static_cast<char>(value >> 8);
                    result.bytes[pos + 1] = static_cast<char>(value);
                } else
                    result.bytes[pos] |= static_cast<char>(value << (8 - result.bits - bit % 8));
            }
    return result;
}
} // namespace dpdfcomp
