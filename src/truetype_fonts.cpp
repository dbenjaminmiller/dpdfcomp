#include "compression.hpp"
#include "font_object.hpp"
#include "font_plan.hpp"
#include "font_usage.hpp"
#include "fonts.hpp"
#include "truetype_font_object.hpp"

#include <algorithm>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{
struct Owner {
    Object font;
    Object descriptor;
    std::vector<Object> parents;
    std::vector<unsigned int> cids;
    bool simple = false;
    std::optional<std::array<unsigned int, 256>> encoding;
    std::optional<std::set<unsigned int>> codes;
};
struct Font {
    Object stream;
    std::vector<Owner> owners;
    std::unique_ptr<TrueTypeFontObject> data;
    std::set<unsigned int> keep;
    bool replaced = false;
    bool merged = false;
    std::string error;
    std::string output_name;
    bool can_merge = true;
};
unsigned int win_ansi_unicode(unsigned int code)
{
    static constexpr unsigned int extended[] = {
        0x20ac, 0x2022, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x2022, 0x017d, 0x2022, 0x2022, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
        0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x2022, 0x017e, 0x0178};
    if (code >= 128 && code < 160)
        return extended[code - 128];
    return code == 127 ? 0x2022 : code == 160 ? 32 : code == 173 ? 45 : code;
}

std::optional<std::array<unsigned int, 256>> simple_encoding(Object font,
                                                             const TrueTypeFontObject& data)
{
    auto flags = font.getKey("/FontDescriptor").getKey("/Flags");
    if (!flags.isInteger() || (flags.getIntValue() & 4) || !(flags.getIntValue() & 32))
        return std::nullopt;
    auto definition = font.getKey("/Encoding");
    auto base = definition.isDictionary() ? definition.getKey("/BaseEncoding") : definition;
    const bool ansi = base.isNameAndEquals("/WinAnsiEncoding");
    if (!ansi && !base.isNameAndEquals("/StandardEncoding"))
        return std::nullopt;
    auto names = ansi ? win_ansi_encoding() : standard_encoding();
    std::set<unsigned int> differences;
    if (definition.isDictionary() && definition.hasKey("/Differences")) {
        auto values = definition.getKey("/Differences");
        if (!values.isArray())
            return std::nullopt;
        int code = -1;
        for (auto value : values.getArrayAsVector()) {
            if (value.isInteger() && value.getIntValue() >= 0 && value.getIntValue() < 256)
                code = value.getIntValue();
            else if (value.isName() && code >= 0 && code < 256) {
                names[code] = value.getName().substr(1);
                differences.insert(code++);
            } else
                return std::nullopt;
        }
    }
    std::array<unsigned int, 256> result{};
    for (unsigned int code = 0; code < 256; ++code) {
        std::optional<unsigned int> glyph;
        if (names[code] == ".notdef")
            glyph = 0;
        else if (ansi && !differences.contains(code))
            glyph = data.unicode_glyph(win_ansi_unicode(code));
        else
            glyph = data.named_glyph(names[code]);
        if (!glyph)
            return std::nullopt;
        result[code] = *glyph;
    }
    return result;
}

void rename(const Font& font, const std::string& name, const Replacements& changes,
            Replacements& proposed)
{
    for (const auto& owner : font.owners) {
        auto descriptor = copy_font_dictionary(owner.descriptor, changes);
        descriptor.replaceKey("/FontName", Object::newName("/" + name));
        proposed[owner.descriptor.getObjGen()] = {descriptor, {}, false};
        auto descendant = copy_font_dictionary(owner.font, changes);
        descendant.replaceKey("/BaseFont", Object::newName("/" + name));
        proposed[owner.font.getObjGen()] = {descendant, {}, false};
        for (auto parent : owner.parents) {
            auto value = copy_font_dictionary(parent, changes);
            value.replaceKey("/BaseFont", Object::newName("/" + name));
            proposed[parent.getObjGen()] = {value, {}, false};
        }
    }
}
std::vector<unsigned int> cid_map(Object font, unsigned int count)
{
    auto map = font.getKey("/CIDToGIDMap");
    std::vector<unsigned int> result(65536);
    if (map.isNull() || map.isNameAndEquals("/Identity")) {
        std::iota(result.begin(), result.begin() + count, 0);
        return result;
    }
    if (!map.isStream())
        throw std::runtime_error("unsupported TrueType CID mapping");
    auto bytes = decoded_bytes(map);
    if (bytes.size() % 2 || bytes.size() > 131072)
        throw std::runtime_error("invalid TrueType CID mapping length");
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        auto gid =
            static_cast<unsigned char>(bytes[i]) * 256u + static_cast<unsigned char>(bytes[i + 1]);
        if (gid >= count)
            throw std::runtime_error("TrueType CID mapping references a missing glyph");
        result[i / 2] = gid;
    }
    return result;
}
Replacement font_stream(Object source, const std::string& bytes)
{
    auto replacement = stream_replacement(source, deflate(bytes), "/FlateDecode");
    replacement.value.replaceKey("/Length1", Object::newInteger(bytes.size()));
    return replacement;
}
} // namespace

void optimize_truetype_fonts(QPDF& pdf, Replacements& changes, const Options& options,
                             Report& report, const FontUsage& usage, Progress& progress)
{
    if (!options.upgrade_version && effective_version(pdf) < PDFVersion(1, 2))
        return;
    const auto objects = pdf.getAllObjects();
    const auto references = reference_counts(pdf);
    std::map<ObjectId, std::vector<Object>> parents;
    for (auto object : objects) {
        if (!object.isDictionary() || !object.getKey("/Subtype").isNameAndEquals("/Type0"))
            continue;
        auto descendants = object.getKey("/DescendantFonts");
        if (descendants.isArray() && descendants.getArrayNItems() == 1)
            parents[descendants.getArrayItem(0).getObjGen()].push_back(object);
    }
    std::vector<Font> fonts;
    auto font_names = embedded_font_names(pdf);
    std::map<ObjectId, std::size_t> positions;
    for (auto object : objects) {
        if (!object.isDictionary())
            continue;
        const bool simple = object.getKey("/Subtype").isNameAndEquals("/TrueType");
        if (!simple && !object.getKey("/Subtype").isNameAndEquals("/CIDFontType2"))
            continue;
        auto descriptor = object.getKey("/FontDescriptor");
        if (!descriptor.isIndirect() || !descriptor.isDictionary())
            continue;
        auto stream = descriptor.getKey("/FontFile2");
        if (!stream.isStream())
            continue;
        auto [position, added] = positions.emplace(stream.getObjGen(), fonts.size());
        if (added) {
            Font font;
            font.stream = stream;
            auto name = descriptor.getKey("/FontName");
            if (name.isName())
                font.output_name = name.getName().substr(1);
            font_names.insert(font.output_name);
            fonts.push_back(std::move(font));
        }
        fonts[position->second].owners.push_back({object,
                                                  descriptor,
                                                  parents[object.getObjGen()],
                                                  {},
                                                  simple,
                                                  std::nullopt,
                                                  usage.codes(object, 1)});
    }
    auto notify = [&] {
        progress.compressed = report.fonts_replaced;
        if (options.progress)
            options.progress(progress);
    };
    auto replaced = [&](Font& font) {
        if (!font.replaced) {
            font.replaced = true;
            ++report.fonts_replaced;
        }
    };
    for (auto& font : fonts) {
        try {
            if (font.output_name.empty())
                throw std::runtime_error("TrueType font has no usable name");
            std::map<ObjectId, std::size_t> descriptors;
            for (const auto& owner : font.owners)
                ++descriptors[owner.descriptor.getObjGen()];
            if (references.at(font.stream.getObjGen()) != descriptors.size())
                throw std::runtime_error("TrueType font has unsupported consumers");
            for (const auto& [id, count] : descriptors)
                if (references.at(id) != count)
                    throw std::runtime_error("TrueType descriptor has unsupported consumers");
            font.data = std::make_unique<TrueTypeFontObject>(decoded_bytes(font.stream));
            bool known = options.subset_fonts;
            for (auto& owner : font.owners) {
                if (owner.simple) {
                    owner.encoding = simple_encoding(owner.font, *font.data);
                    font.can_merge &= owner.encoding.has_value();
                    if (!owner.encoding || !owner.codes)
                        known = false;
                    else
                        for (auto code : *owner.codes)
                            font.keep.insert((*owner.encoding)[code]);
                    continue;
                }
                owner.cids = cid_map(owner.font, font.data->glyph_count());
                if (owner.parents.empty())
                    known = false;
                for (auto parent : owner.parents) {
                    auto encoding = parent.getKey("/Encoding");
                    if (!encoding.isNameAndEquals("/Identity-H") &&
                        !encoding.isNameAndEquals("/Identity-V"))
                        known = false;
                    auto codes = usage.codes(parent, 2);
                    if (!codes)
                        known = false;
                    else
                        for (auto code : *codes)
                            font.keep.insert(owner.cids[code]);
                }
            }
            if (!known)
                for (unsigned int i = 0; i < font.data->glyph_count(); ++i)
                    font.keep.insert(i);
            font.keep = font.data->closure(std::move(font.keep));
            if (font.keep.size() < font.data->glyph_count())
                font.output_name =
                    subset_font_name(font.output_name, font.stream.getObjGen(), font_names);
            auto bytes = font.data->subset(font.keep, font.output_name);
            TrueTypeFontObject candidate(bytes);
            TrueTypeFontObject::GlyphMap identity;
            for (auto gid : font.keep)
                identity[gid] = gid;
            if (!candidate.preserves(*font.data, font.keep, identity))
                throw std::runtime_error(
                    "TrueType subset changed outlines, instructions or metrics");
            Replacements proposed{{font.stream.getObjGen(), font_stream(font.stream, bytes)}};
            rename(font, font.output_name, changes, proposed);
            if (auto saved = apply_font_plan(pdf, changes, std::move(proposed))) {
                progress.bytes_saved += saved;
                replaced(font);
            }
        } catch (const std::exception& error) {
            font.error = error.what();
            report.notes.push_back("font " + object_label(font.stream.getObjGen()) + ": " +
                                   font.error);
        }
        ++progress.processed;
        notify();
    }
    for (std::size_t i = 0; options.merge_fonts && i < fonts.size(); ++i) {
        auto& first = fonts[i];
        if (!first.error.empty() || first.merged || !first.can_merge)
            continue;
        std::vector<std::size_t> members{i};
        std::vector<const TrueTypeFontObject*> sources{first.data.get()};
        std::vector<std::set<unsigned int>> keep{first.keep};
        for (std::size_t j = i + 1; j < fonts.size(); ++j) {
            const auto& other = fonts[j];
            if (!other.error.empty() || other.merged || !other.can_merge ||
                !first.data->compatible(*other.data))
                continue;
            members.push_back(j);
            sources.push_back(other.data.get());
            keep.push_back(other.keep);
        }
        if (members.size() == 1)
            continue;
        try {
            bool cid = true;
            for (auto index : members)
                for (const auto& owner : fonts[index].owners)
                    cid &= !owner.simple;
            auto output = TrueTypeFontObject::merge(sources, keep, cid, first.output_name);
            TrueTypeFontObject candidate(output.bytes);
            for (std::size_t n = 0; n < members.size(); ++n)
                if (!candidate.preserves(*sources[n], keep[n], output.mappings[n]))
                    throw std::runtime_error(
                        "TrueType merge changed outlines, instructions or metrics");
            for (std::size_t n = 0; n < members.size(); ++n)
                for (const auto& owner : fonts[members[n]].owners) {
                    if (!owner.simple)
                        continue;
                    auto after = simple_encoding(owner.font, candidate);
                    if (!after)
                        throw std::runtime_error("TrueType merge changed the encoding");
                    for (unsigned int code = 0; code < 256; ++code) {
                        if (owner.codes && !owner.codes->contains(code))
                            continue;
                        auto expected = output.mappings[n].find((*owner.encoding)[code]);
                        if (expected == output.mappings[n].end() ||
                            (*after)[code] != expected->second)
                            throw std::runtime_error("TrueType merge changed a character mapping");
                    }
                }
            Replacements proposed{
                {first.stream.getObjGen(), font_stream(first.stream, output.bytes)}};
            std::vector<FontMappingStream> mappings;
            auto font_name = Object::newName("/" + first.output_name);
            for (std::size_t n = 0; n < members.size(); ++n) {
                auto& font = fonts[members[n]];
                if (n)
                    proposed[font.stream.getObjGen()] = {Object::newNull(), {}, false};
                for (const auto& owner : font.owners) {
                    auto descriptor = copy_font_dictionary(owner.descriptor, changes);
                    descriptor.replaceKey("/FontFile2", first.stream);
                    descriptor.replaceKey("/FontName", font_name);
                    proposed[owner.descriptor.getObjGen()] = {descriptor, {}, false};
                    auto descendant = copy_font_dictionary(owner.font, changes);
                    descendant.replaceKey("/BaseFont", font_name);
                    if (!owner.simple)
                        descendant.removeKey("/CIDToGIDMap");
                    proposed[owner.font.getObjGen()] = {descendant, {}, false};
                    for (auto parent : owner.parents) {
                        auto dictionary = copy_font_dictionary(parent, changes);
                        dictionary.replaceKey("/BaseFont", font_name);
                        proposed[parent.getObjGen()] = {dictionary, {}, false};
                    }
                    if (owner.simple)
                        continue;
                    std::string map;
                    for (auto old : owner.cids) {
                        auto found = output.mappings[n].find(old);
                        unsigned int gid = found == output.mappings[n].end() ? 0 : found->second;
                        map.push_back(static_cast<char>(gid >> 8));
                        map.push_back(static_cast<char>(gid));
                    }
                    while (map.size() > 2 && map[map.size() - 2] == 0 && map.back() == 0)
                        map.resize(map.size() - 2);
                    mappings.push_back({deflate(map), {{owner.font.getObjGen(), "/CIDToGIDMap"}}});
                }
            }
            auto saved = apply_font_plan(pdf, changes, std::move(proposed), std::move(mappings));
            if (!saved)
                continue;
            progress.bytes_saved += saved;
            report.fonts_merged += members.size() - 1;
            for (auto index : members) {
                fonts[index].merged = true;
                replaced(fonts[index]);
            }
            notify();
        } catch (const std::exception& error) {
            report.notes.push_back("TrueType merge " + object_label(first.stream.getObjGen()) +
                                   ": " + error.what());
        }
    }
}
} // namespace dpdfcomp
