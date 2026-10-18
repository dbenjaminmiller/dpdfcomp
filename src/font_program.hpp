#pragma once

#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dpdfcomp
{

// Adobe's reader contexts and callbacks are confined to this implementation.
class FontProgram
{
  public:
    struct Glyph {
        float width = 0;
        std::vector<float> drawing;
        std::vector<float> hints;
        std::set<std::string> dependencies;
        bool operator==(const Glyph&) const = default;
    };
    FontProgram(std::string data, bool type1);
    ~FontProgram();
    FontProgram(const FontProgram&) = delete;
    FontProgram& operator=(const FontProgram&) = delete;

    const std::map<std::string, Glyph>& glyphs() const;
    const std::array<std::string, 256>& encoding() const;
    std::string name() const;
    bool compatible(const FontProgram& other) const;
    bool has_shared_outlines(const FontProgram& other) const;
    std::set<std::string> closure(std::set<std::string> keep) const;

    // A null keep-set means every glyph. Sources must have compatible rendering
    // dictionaries and identical same-named glyphs; the caller decides that policy.
    static std::string cff(const std::vector<FontProgram*>& sources,
                           const std::set<std::string>* keep = nullptr,
                           const std::string& name = {});
    bool preserves(const FontProgram& source, const std::set<std::string>& keep) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const std::array<std::string, 256>& standard_encoding();

} // namespace dpdfcomp
