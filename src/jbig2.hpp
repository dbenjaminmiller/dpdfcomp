#pragma once

#include "image_geometry.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace dpdfcomp
{
// Leptonica's packed, MSB-first words; row padding must be zero. Each stripe
// becomes an independent generic region inside the same PDF image stream.
std::string encode_jbig2(const std::uint32_t* pixels, int width, int height, int stripe_rows,
                         std::optional<ImageBounds> bounds = std::nullopt);
} // namespace dpdfcomp
