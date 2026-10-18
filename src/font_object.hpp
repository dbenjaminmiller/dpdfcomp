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
class FontObject
{
  public:
    struct Glyph {
        float width = 0;
        std::vector<float> drawing;
        std::vector<float> hints;
        std::set<std::string> dependencies;
        bool operator==(const Glyph&) const = default;
    };
    FontObject(std::string data, bool type1);
    ~FontObject();
    FontObject(const FontObject&) = delete;
    FontObject& operator=(const FontObject&) = delete;

    const std::map<std::string, Glyph>& glyphs() const;
    const std::array<std::string, 256>& encoding() const;
    std::string name() const;
    bool cid_keyed() const;
    bool compatible(const FontObject& other) const;
    bool has_shared_outlines(const FontObject& other) const;
    std::set<std::string> closure(std::set<std::string> keep) const;

    // A null keep-set means every glyph. Sources must have compatible rendering
    // dictionaries and identical same-named glyphs; the caller decides that policy.
    static std::string
    cff(const std::vector<FontObject*>& sources, const std::set<std::string>* keep = nullptr,
        const std::string& name = {},
        const std::vector<std::map<std::string, std::string>>* mappings = nullptr);
    bool preserves(const FontObject& source, const std::set<std::string>& keep,
                   const std::map<std::string, std::string>* mapping = nullptr) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const std::array<std::string, 256>& standard_encoding();
const std::array<std::string, 256>& win_ansi_encoding();
std::string cid_name(unsigned int cid);

} // namespace dpdfcomp
