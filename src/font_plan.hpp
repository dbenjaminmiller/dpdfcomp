#pragma once

#include "pdf.hpp"
#include <set>
#include <vector>

namespace dpdfcomp
{
// Mapping streams are allocated only when the complete font replacement saves space.
struct FontMappingStream {
    std::string bytes;
    std::vector<std::pair<ObjectId, std::string>> references;
    std::map<std::string, Object> entries{};
};
Object copy_font_dictionary(Object object, const Replacements& changes);
std::size_t apply_font_plan(QPDF& pdf, Replacements& changes, Replacements proposed,
                            std::vector<FontMappingStream> streams = {});
std::set<std::string> embedded_font_names(QPDF& pdf);
std::string declared_font_name(Object descriptor, const std::string& fallback);
std::string subset_font_name(const std::string& name, ObjectId id, std::set<std::string>& names);
} // namespace dpdfcomp
