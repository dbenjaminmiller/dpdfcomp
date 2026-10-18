#pragma once

#include "pdf.hpp"
#include <optional>
#include <set>

namespace dpdfcomp
{
// Text bytes are collected with their actual font resource. Unknown contexts
// retain the whole font instead of guessing which glyphs can be discarded.
class FontUsage
{
  public:
    explicit FontUsage(QPDF& pdf);
    std::optional<std::set<unsigned int>> codes(Object font, int bytes_per_code) const;

  private:
    bool complete_ = true;
    std::map<ObjectId, std::set<std::string>> strings_;
};
} // namespace dpdfcomp
