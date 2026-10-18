#pragma once
#include "pdf.hpp"
#include "raster.hpp"
#include <vector>

namespace dpdfcomp
{
struct ReconstructedImage {
    Object prototype;
    ObjectId target;
    int rows = 0;
    Replacements related;
    std::size_t original_size = 0;
    std::size_t related_size = 0;
    std::vector<ObjectId> strips; // Top to bottom; decoded lazily in the pipeline.
};
// Recognizes simple, isolated runs of image draws. Unknown painting operators,
// shared resources, masks, gaps, overlaps, and mismatched attributes are rejected.
std::vector<ReconstructedImage> reconstruct_images(QPDF&);
Raster read_reconstructed_image(QPDF&, const ReconstructedImage&);
} // namespace dpdfcomp
