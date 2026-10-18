#include "core/fxcodec/jbig2/jbig2_decoder.h"
#include "core/fxcodec/jbig2/jbig2_document_context.h"
#include "image_codecs.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dpdfcomp
{
std::string decode_jbig2(const std::string& bytes, const std::string& globals, int width,
                         int height)
{
    const auto pitch = ((std::size_t(width) + 31) / 32) * 4;
    if (pitch > std::numeric_limits<std::size_t>::max() / height ||
        pitch > std::numeric_limits<unsigned>::max())
        throw std::runtime_error("JBIG2 image dimensions overflow");
    std::vector<std::uint8_t> pixels(pitch * height);
    JBig2_DocumentContext document;
    Jbig2Context context;
    auto status = Jbig2Decoder::StartDecode(
        &context, &document, width, height,
        pdfium::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), 0,
        pdfium::span(reinterpret_cast<const std::uint8_t*>(globals.data()), globals.size()), 0,
        pdfium::span(pixels), static_cast<unsigned>(pitch), nullptr, false);
    while (status == FXCODEC_STATUS::kDecodeToBeContinued)
        status = Jbig2Decoder::ContinueDecode(&context, nullptr);
    if (status != FXCODEC_STATUS::kDecodeFinished)
        throw std::runtime_error("JBIG2 decoding failed");
    const auto row_bytes = (std::size_t(width) + 7) / 8;
    std::string result(row_bytes * height, '\0');
    for (int y = 0; y < height; ++y)
        std::copy_n(pixels.data() + std::size_t(y) * pitch, row_bytes,
                    result.data() + std::size_t(y) * row_bytes);
    return result;
}
} // namespace dpdfcomp
