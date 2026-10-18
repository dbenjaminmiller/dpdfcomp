#pragma once
#include "raster.hpp"
#include <array>
#include <vector>

namespace dpdfcomp
{
// Only device gray/RGB with endpoint Decode mappings are eligible. All values
// are copied out of the PDF before parallel pixel work begins.
struct BilevelInput {
    int colors = 0;
    std::array<bool, 3> reversed{};
    std::vector<unsigned> color_key;
    std::optional<Raster> alpha;
    bool alpha_reversed = false;
};
struct BilevelResult {
    Raster raster;
    std::optional<std::array<int, 2>> color_key;
    std::optional<std::string> mask; // Explicit stencil: 1 transparent, 0 opaque.
};
std::optional<BilevelResult> convert_bilevel(const Raster&, const BilevelInput&,
                                             std::optional<int> threshold);
} // namespace dpdfcomp
