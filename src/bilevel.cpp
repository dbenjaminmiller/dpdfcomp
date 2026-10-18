#include "bilevel.hpp"
#include <algorithm>

namespace dpdfcomp
{
std::optional<BilevelResult> convert_bilevel(const Raster& source, const BilevelInput& input,
                                             std::optional<int> threshold)
{
    if (!input.colors)
        return std::nullopt;
    BilevelResult result{Raster{source.width, source.height, 1, 1, {}, {}}, {}, {}};
    auto& raster = result.raster;
    raster.bytes.resize(raster.row_bytes() * raster.height);
    const unsigned maximum = (1U << source.bits) - 1;
    std::string mask(input.color_key.empty() ? 0 : raster.bytes.size(), '\0');
    std::array<bool, 2> transparent{}, opaque{};
    for (int y = 0; y < source.height; ++y)
        for (int x = 0; x < source.width; ++x) {
            std::array<unsigned, 3> values{};
            bool hidden = !input.color_key.empty();
            for (int c = 0; c < source.colors; ++c) {
                auto value = sample(source, x, y, c);
                if (!input.color_key.empty())
                    hidden &=
                        value >= input.color_key[2 * c] && value <= input.color_key[2 * c + 1];
                values[c] = input.reversed[c] ? maximum - value : value;
            }
            if (input.alpha) {
                const auto alpha = sample(*input.alpha, x, y, 0);
                hidden = input.alpha_reversed ? alpha == (1U << input.alpha->bits) - 1 : alpha == 0;
            }
            bool white = false;
            if (!hidden) {
                if (threshold) {
                    // Rec. 601 luma on decoded device samples, with white at or above the chosen
                    // threshold.
                    const auto luma = source.colors == 1
                                          ? 1000U * values[0]
                                          : 299U * values[0] + 587U * values[1] + 114U * values[2];
                    white = std::uint64_t(100) * luma >= std::uint64_t(1000) * maximum * *threshold;
                } else {
                    white = values[0] == maximum;
                    for (int c = 0; c < source.colors; ++c)
                        if (values[c] != (white ? maximum : 0))
                            return std::nullopt;
                }
            }
            const auto pos = std::size_t(y) * raster.row_bytes() + x / 8;
            const auto bit = static_cast<char>(128 >> (x % 8));
            if (white)
                raster.bytes[pos] |= bit;
            if (!input.color_key.empty()) {
                (hidden ? transparent : opaque)[white] = true;
                if (hidden)
                    mask[pos] |= bit;
            }
        }
    if (!input.color_key.empty()) {
        if ((transparent[0] && opaque[0]) || (transparent[1] && opaque[1]))
            result.mask = std::move(mask);
        else if (transparent[0] || transparent[1])
            result.color_key = std::array<int, 2>{transparent[0] ? 0 : 1, transparent[1] ? 1 : 0};
        // No matching source samples: remove the color key rather than
        // accidentally making a newly introduced black or white value invisible.
    }
    return result;
}
} // namespace dpdfcomp
