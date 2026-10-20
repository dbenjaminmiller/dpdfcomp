#pragma once

#include "bilevel.hpp"
#include "pdf.hpp"
#include "raster.hpp"
#include <dpdfcomp/optimizer.hpp>
#include <set>

namespace dpdfcomp
{
// QPDF access belongs to the serial pipeline stages. The returned raster and
// conversion metadata own their data and can safely enter parallel codec work.
int components(Object color_space);
Raster read_image(Object source);
bool lossy_input(Object filter);
bool matches_codec(Object source, const Options&, bool bilevel,
                   const std::vector<ColorEncoding>& color);
BilevelInput bilevel_input(Object source, const Raster&);
std::optional<std::string> white_pixel(Object source, const Raster&);
// Images whose samples define transparency rather than color: image soft masks,
// and images painted within luminosity soft-mask groups.
std::set<ObjectId> transparency_masks(QPDF&);
} // namespace dpdfcomp
