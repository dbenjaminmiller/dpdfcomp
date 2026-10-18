#include "font_plan.hpp"

#include <algorithm>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{
bool has_subset_tag(const std::string& name)
{
    return name.size() > 7 && name[6] == '+' &&
           std::all_of(name.begin(), name.begin() + 6, [](char c) { return c >= 'A' && c <= 'Z'; });
}
} // namespace
Object copy_font_dictionary(Object object, const Replacements& changes)
{
    auto prior = changes.find(object.getObjGen());
    auto value = prior == changes.end() ? object : prior->second.value;
    return value.shallowCopy();
}

std::size_t apply_font_plan(QPDF& pdf, Replacements& changes, Replacements proposed,
                            std::vector<FontMappingStream> streams)
{
    std::size_t before = 0, after = 0;
    for (const auto& [id, replacement] : proposed) {
        auto prior = changes.find(id);
        before += prior == changes.end() ? stored_size(pdf.getObjectByObjGen(id))
                                         : stored_size(prior->second);
        after += stored_size(replacement);
    }
    // Account conservatively for each new stream's dictionary and references.
    for (const auto& stream : streams)
        after += stream.bytes.size() +
                 Object::newDictionary(stream.entries).unparseResolved().size() + 128 +
                 64 * stream.references.size();
    if (after >= before)
        return 0;
    for (const auto& stream : streams) {
        auto object = pdf.newStream(stream.bytes);
        for (const auto& [key, value] : stream.entries)
            object.getDict().replaceKey(key, value);
        object.getDict().replaceKey("/Filter", Object::newName("/FlateDecode"));
        for (const auto& [owner, key] : stream.references)
            proposed.at(owner).value.replaceKey(key, object);
    }
    for (auto& [id, replacement] : proposed)
        changes[id] = std::move(replacement);
    return before - after;
}

std::set<std::string> embedded_font_names(QPDF& pdf)
{
    std::set<std::string> names;
    for (auto object : pdf.getAllObjects())
        if (object.isDictionary())
            for (const auto* key : {"/FontName", "/BaseFont"}) {
                auto name = object.getKey(key);
                if (name.isName())
                    names.insert(name.getName().substr(1));
            }
    return names;
}

std::string subset_font_name(const std::string& name, ObjectId object, std::set<std::string>& names)
{
    if (has_subset_tag(name))
        return name;
    auto seed = static_cast<unsigned int>(object.getObj());
    std::string result;
    do {
        if (seed >= 308915776U)
            throw std::runtime_error("font ID exceeds subset tag range");
        std::string tag(6, 'A');
        auto value = seed++;
        for (auto& c : tag) {
            c = static_cast<char>('A' + value % 26);
            value /= 26;
        }
        result = tag + '+' + name;
    } while (!names.insert(result).second);
    return result;
}

std::string declared_font_name(Object descriptor, const std::string& fallback)
{
    // Some producers put the subset tag only in the PDF descriptor. Keep it
    // when naming a rewritten font instead of making the subset look complete.
    auto name = descriptor.getKey("/FontName");
    if (name.isName() && has_subset_tag(name.getName().substr(1)))
        return name.getName().substr(1);
    return fallback;
}
} // namespace dpdfcomp
