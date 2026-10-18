#pragma once

#include "image_geometry.hpp"
#include "pdf.hpp"
#include <set>
#include <vector>

namespace dpdfcomp
{
// Images may become forms only when every indirect use is an XObject resource.
std::set<ObjectId> form_compatible_images(QPDF& pdf, bool include_forms = false);

struct ImagePart {
    Replacement image;
    std::vector<ImageBounds> placements; // Original pixel coordinates, from the top left.
};
struct ImageForm {
    Replacement form;
    std::vector<ImagePart> parts;
    std::size_t size_bound;
};
// Prepare without allocating PDF objects, then commit only the chosen candidate.
ImageForm make_image_form(int width, int height, std::vector<ImagePart> parts);
std::size_t commit_image_form(QPDF& pdf, Replacements& changes, Object source, ImageForm form);
} // namespace dpdfcomp
