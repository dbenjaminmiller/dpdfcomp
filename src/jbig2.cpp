#include "jbig2.hpp"

#include <jbig2arith.h>
#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace dpdfcomp
{
namespace
{
struct ArithmeticEncoder {
    jbig2enc_ctx context{};
    ArithmeticEncoder() { jbig2enc_init(&context); }
    ~ArithmeticEncoder() { jbig2enc_dealloc(&context); }
    ArithmeticEncoder(const ArithmeticEncoder&) = delete;
    ArithmeticEncoder& operator=(const ArithmeticEncoder&) = delete;
};

void integer(std::string& bytes, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<char>((value >> shift) & 0xff));
}

void segment(std::string& stream, std::uint32_t number, char type, std::string_view body)
{
    if (body.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("JBIG2 segment exceeds its format's size limit");
    integer(stream, number);
    stream.push_back(type);
    stream.push_back(0); // No segment references or retained segments.
    stream.push_back(1); // PDF's embedded organization always uses page 1.
    integer(stream, static_cast<std::uint32_t>(body.size()));
    stream += body;
}

std::string region(const std::uint32_t* pixels, int width, int height, int x, int y)
{
    ArithmeticEncoder encoder;
    jbig2enc_bitimage(&encoder.context, reinterpret_cast<const std::uint8_t*>(pixels), width,
                      height, false);
    jbig2enc_final(&encoder.context);
    std::string body;
    integer(body, width);
    integer(body, height);
    integer(body, x);
    integer(body, y);
    body.push_back(0); // OR composition; the regions do not overlap.
    body.push_back(0); // Arithmetic coding, template 0, no typical prediction.
    body.append("\x03\xff\xfd\xff\x02\xfe\xfe\xfe", 8); // Template 0 AT pixels.
    const auto offset = body.size();
    body.resize(offset + jbig2enc_datasize(&encoder.context));
    jbig2enc_tobuffer(&encoder.context, reinterpret_cast<std::uint8_t*>(body.data() + offset));
    return body;
}
} // namespace

std::string encode_jbig2(const std::uint32_t* pixels, int width, int height, int stripe_rows,
                         std::optional<ImageBounds> bounds)
{
    if (!pixels || width <= 0 || height <= 0 || stripe_rows < 0)
        throw std::invalid_argument("invalid JBIG2 bitmap or stripe height");
    const auto area = bounds.value_or(ImageBounds{0, 0, width, height});
    if (area.x < 0 || area.y < 0 || area.width < 0 || area.height < 0 || area.x > width ||
        area.y > height || area.width > width - area.x || area.height > height - area.y)
        throw std::invalid_argument("invalid JBIG2 region bounds");
    const auto rows = stripe_rows ? std::min(stripe_rows, area.height) : area.height;
    const auto count = area.width && area.height ? 1 + (area.height - 1) / rows : 0;
    const auto source_stride = (std::size_t(width) + 31) / 32;
    const auto stride = (std::size_t(area.width) + 31) / 32;
    std::vector<std::uint32_t> cropped;
    if (bounds && count) {
        cropped.resize(stride * area.height);
        for (int y = 0; y < area.height; ++y)
            for (int x = 0; x < area.width; ++x)
                if (pixels[std::size_t(y + area.y) * source_stride + (x + area.x) / 32] &
                    (std::uint32_t(0x80000000) >> ((x + area.x) % 32)))
                    cropped[std::size_t(y) * stride + x / 32] |=
                        std::uint32_t(0x80000000) >> (x % 32);
        pixels = cropped.data();
    }
    std::vector<std::string> regions(count);
    oneapi::tbb::parallel_for(0, count, [&](int i) {
        const auto y = i * rows;
        regions[i] = region(pixels + std::size_t(y) * stride, area.width,
                            std::min(rows, area.height - y), area.x, area.y + y);
    });
    std::string page;
    integer(page, width);
    integer(page, height);
    integer(page, 0); // Resolution is supplied by the PDF's image placement.
    integer(page, 0);
    page.push_back(1);    // Lossless, initially white, OR composition.
    page.append(2, '\0'); // Known full height; no end-of-stripe markers are needed.
    std::string stream;
    segment(stream, 0, 48, page);
    for (int i = 0; i < count; ++i)
        segment(stream, i + 1, 38, regions[i]);
    // PDF prohibits JBIG2 file headers and end-of-page/end-of-file segments.
    return stream;
}
} // namespace dpdfcomp
