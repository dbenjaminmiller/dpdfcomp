#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dpdfcomp
{
// Static TrueType fonts with glyf outlines. Glyph instructions remain byte-for-byte intact;
// merging changes only composite references and the PDF's CID-to-GID maps.
class TrueTypeFontObject
{
  public:
    using GlyphMap = std::map<unsigned int, unsigned int>;
    struct Result {
        std::string bytes;
        std::vector<GlyphMap> mappings;
    };
    explicit TrueTypeFontObject(std::string bytes);
    unsigned int glyph_count() const;
    bool compatible(const TrueTypeFontObject& other) const;
    std::optional<unsigned int> named_glyph(const std::string& name) const;
    std::optional<unsigned int> unicode_glyph(unsigned int codepoint) const;
    std::set<unsigned int> closure(std::set<unsigned int> keep) const;
    std::string subset(const std::set<unsigned int>& keep, const std::string& name = {}) const;
    static Result merge(const std::vector<const TrueTypeFontObject*>& sources,
                        const std::vector<std::set<unsigned int>>& keep, bool cid = true,
                        const std::string& name = {});
    bool preserves(const TrueTypeFontObject& source, const std::set<unsigned int>& keep,
                   const GlyphMap& mapping) const;

  private:
    struct Glyph {
        std::string bytes;
        std::string horizontal;
        std::string vertical;
        std::vector<std::size_t> component_offsets;
    };
    std::map<std::string, std::string> tables_;
    std::vector<Glyph> glyphs_;
    std::map<unsigned int, unsigned int> unicode_;
    std::map<std::string, unsigned int> names_;
    bool unicode_available_ = false;
    bool names_available_ = false;
};
} // namespace dpdfcomp
