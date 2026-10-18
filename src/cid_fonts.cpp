#include "compression.hpp"
#include "font_object.hpp"
#include "font_plan.hpp"
#include "font_usage.hpp"
#include "fonts.hpp"

#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{
using GlyphMap = std::map<std::string, std::string>;
struct Owner {
    Object font;
    Object descriptor;
    std::vector<Object> parents;
};
struct Font {
    Object stream;
    std::vector<Owner> owners;
    std::unique_ptr<FontObject> data;
    std::set<std::string> keep;
    bool can_merge = true;
    bool replaced = false;
    bool merged = false;
    std::string error;
    std::string output_name;
};

unsigned int cid(const std::string& name)
{
    return name == ".notdef" ? 0 : static_cast<unsigned int>(std::stoul(name.substr(3)));
}

FontMappingStream cmap(const GlyphMap& mapping, Object system, bool vertical,
                       const std::string& name)
{
    if (!system.isDictionary() || !system.getKey("/Registry").isString() ||
        !system.getKey("/Ordering").isString() || !system.getKey("/Supplement").isInteger())
        throw std::runtime_error("unsupported CID system information");
    auto direct = Object::newDictionary(
        {{"/Registry", Object::newString(system.getKey("/Registry").getStringValue())},
         {"/Ordering", Object::newString(system.getKey("/Ordering").getStringValue())},
         {"/Supplement", Object::newInteger(system.getKey("/Supplement").getIntValue())}});
    std::ostringstream out;
    // CMaps use a restricted PostScript syntax, not PDF dictionary syntax.
    // In particular, Quartz rejects << ... >> here even though other readers
    // accept it. Keep the stream body and required PDF dictionary in agreement.
    out << "%!PS-Adobe-3.0 Resource-CMap\n"
           "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
           "/CIDSystemInfo 3 dict dup begin\n/Registry "
        << direct.getKey("/Registry").unparseResolved() << " def\n/Ordering "
        << direct.getKey("/Ordering").unparseResolved() << " def\n/Supplement "
        << direct.getKey("/Supplement").unparseResolved() << " def\nend def\n/CMapName " << name
        << " def\n/CMapType 1 def\n/WMode " << vertical
        << " def\n1 begincodespacerange\n<0000> <ffff>\nendcodespacerange\n";
    std::map<unsigned int, unsigned int> codes;
    for (const auto& [from, to] : mapping)
        codes.emplace(cid(from), cid(to));
    for (auto it = codes.begin(); it != codes.end();) {
        const auto count = std::min<std::size_t>(100, std::distance(it, codes.end()));
        out << count << " begincidchar\n";
        for (std::size_t i = 0; i < count; ++i, ++it)
            out << '<' << std::hex << std::setw(4) << std::setfill('0') << it->first << "> "
                << std::dec << it->second << '\n';
        out << "endcidchar\n";
    }
    out << "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    FontMappingStream result;
    result.bytes = deflate(out.str());
    result.entries = {{"/Type", Object::newName("/CMap")},
                      {"/CMapName", Object::newName(name)},
                      {"/CIDSystemInfo", direct},
                      {"/WMode", Object::newInteger(vertical)}};
    return result;
}

using Metrics = std::map<unsigned int, std::vector<Object>>;
Metrics metrics(Object array, int count)
{
    Metrics result;
    if (array.isNull())
        return result;
    if (!array.isArray())
        throw std::runtime_error("invalid CID widths");
    auto items = array.getArrayAsVector();
    auto number = [](Object object) {
        if (!object.isInteger() || object.getIntValue() < 0 || object.getIntValue() > 65535)
            throw std::runtime_error("invalid CID width range");
        return static_cast<unsigned int>(object.getIntValue());
    };
    for (std::size_t i = 0; i < items.size();) {
        auto first = number(items[i++]);
        if (i == items.size())
            throw std::runtime_error("truncated CID widths");
        if (items[i].isArray()) {
            auto values = items[i++].getArrayAsVector();
            if (values.size() % count || values.size() / count > 65536 - first)
                throw std::runtime_error("invalid CID width list");
            for (std::size_t j = 0; j < values.size(); j += count, ++first)
                result[first] = {values.begin() + j, values.begin() + j + count};
        } else {
            auto last = number(items[i++]);
            if (last < first || items.size() - i < static_cast<std::size_t>(count))
                throw std::runtime_error("invalid CID width range");
            const std::vector<Object> value(items.begin() + i, items.begin() + i + count);
            i += count;
            for (auto code = first; code <= last; ++code)
                result[code] = value;
        }
    }
    for (const auto& [code, values] : result)
        for (auto value : values)
            if (!value.isNumber())
                throw std::runtime_error("invalid CID width value");
    return result;
}

void remap_widths(Object font, const GlyphMap& mapping, bool vertical)
{
    auto widths = metrics(font.getKey("/W"), 1);
    auto default_width = font.getKey("/DW");
    if (default_width.isNull())
        default_width = Object::newInteger(1000);
    if (!default_width.isNumber())
        throw std::runtime_error("invalid default CID width");
    auto output = Object::newArray();
    for (const auto& [old_name, new_name] : mapping) {
        auto found = widths.find(cid(old_name));
        auto width = found == widths.end() ? default_width : found->second[0];
        output.appendItem(Object::newInteger(cid(new_name)));
        output.appendItem(Object::newArray({width}));
    }
    font.replaceKey("/W", output);
    if (!vertical && !font.hasKey("/W2"))
        return;
    auto values = metrics(font.getKey("/W2"), 3);
    auto defaults = font.getKey("/DW2");
    if (defaults.isNull())
        defaults = Object::parse("[880 -1000]");
    if (!defaults.isArray() || defaults.getArrayNItems() != 2 ||
        !defaults.getArrayItem(0).isNumber() || !defaults.getArrayItem(1).isNumber())
        throw std::runtime_error("invalid default vertical CID metrics");
    output = Object::newArray();
    for (const auto& [old_name, new_name] : mapping) {
        auto found = values.find(cid(old_name));
        std::vector<Object> value;
        if (found != values.end())
            value = found->second;
        else {
            auto w = widths.find(cid(old_name));
            auto width = w == widths.end() ? default_width : w->second[0];
            if (!width.isInteger() || width.getIntValue() < -1000000000 ||
                width.getIntValue() > 1000000000)
                throw std::runtime_error("noninteger default vertical glyph origin");
            value = {defaults.getArrayItem(1), Object::newReal(width.getNumericValue() / 2, 1),
                     defaults.getArrayItem(0)};
        }
        output.appendItem(Object::newInteger(cid(new_name)));
        output.appendItem(Object::newArray(value));
    }
    font.replaceKey("/W2", output);
}

void rename(Font& source, const FontObject& output, const Replacements& changes,
            Replacements& proposed)
{
    for (const auto& owner : source.owners) {
        auto descriptor = copy_font_dictionary(owner.descriptor, changes);
        descriptor.replaceKey("/FontName", Object::newName("/" + output.name()));
        proposed[owner.descriptor.getObjGen()] = {descriptor, {}, false};
        auto font = copy_font_dictionary(owner.font, changes);
        font.replaceKey("/BaseFont", Object::newName("/" + output.name()));
        proposed[owner.font.getObjGen()] = {font, {}, false};
        for (auto parent : owner.parents) {
            auto value = copy_font_dictionary(parent, changes);
            value.replaceKey("/BaseFont", Object::newName("/" + output.name()));
            proposed[parent.getObjGen()] = {value, {}, false};
        }
    }
}
} // namespace

void optimize_cid_fonts(QPDF& pdf, Replacements& changes, const Options& options, Report& report,
                        const FontUsage& usage, Progress& progress)
{
    if (!options.upgrade_version && effective_version(pdf) < PDFVersion(1, 2))
        return;
    const auto objects = pdf.getAllObjects();
    const auto references = reference_counts(pdf);
    std::set<std::string> cmap_names;
    for (auto object : objects) {
        auto dictionary = object.isStream() ? object.getDict() : object;
        if (!dictionary.isDictionary())
            continue;
        for (const auto* key : {"/CMapName", "/Encoding"}) {
            auto name = dictionary.getKey(key);
            if (name.isName())
                cmap_names.insert(name.getName());
        }
    }
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
        if (!object.isDictionary() || !object.getKey("/Subtype").isNameAndEquals("/CIDFontType0"))
            continue;
        auto descriptor = object.getKey("/FontDescriptor");
        if (!descriptor.isIndirect() || !descriptor.isDictionary())
            continue;
        auto stream = descriptor.getKey("/FontFile3");
        if (!stream.isStream() ||
            !stream.getDict().getKey("/Subtype").isNameAndEquals("/CIDFontType0C"))
            continue;
        auto [position, added] = positions.emplace(stream.getObjGen(), fonts.size());
        if (added) {
            Font font;
            font.stream = stream;
            fonts.push_back(std::move(font));
        }
        fonts[position->second].owners.push_back({object, descriptor, parents[object.getObjGen()]});
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
            std::map<ObjectId, std::size_t> descriptors;
            for (const auto& owner : font.owners)
                ++descriptors[owner.descriptor.getObjGen()];
            if (references.at(font.stream.getObjGen()) != descriptors.size())
                throw std::runtime_error("CID embedded font has unsupported consumers");
            for (const auto& [id, count] : descriptors)
                if (references.at(id) != count)
                    throw std::runtime_error("CID descriptor has unsupported consumers");
            font.data = std::make_unique<FontObject>(decoded_bytes(font.stream), false);
            if (!font.data->cid_keyed())
                throw std::runtime_error("CID font contains name-keyed CFF data");
            font.output_name =
                declared_font_name(font.owners.front().descriptor, font.data->name());
            if (font.output_name.empty())
                throw std::runtime_error("CID font has no usable name");
            font_names.insert(font.output_name);
            font.keep.insert(".notdef");
            bool known = true;
            for (const auto& owner : font.owners) {
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
                        for (auto code : *codes) {
                            font.keep.insert(cid_name(code));
                            if (!font.data->glyphs().contains(cid_name(code)))
                                font.can_merge = false;
                        }
                }
            }
            font.can_merge &= known;
            if (!known || !options.subset_fonts)
                for (const auto& [name, glyph] : font.data->glyphs())
                    font.keep.insert(name);
            if (font.keep.size() < font.data->glyphs().size())
                font.output_name =
                    subset_font_name(font.output_name, font.stream.getObjGen(), font_names);
            auto bytes = FontObject::cff({font.data.get()}, &font.keep, font.output_name);
            FontObject candidate(bytes, false);
            if (!candidate.preserves(*font.data, font.keep))
                throw std::runtime_error("CID CFF round-trip changed glyphs or hints");
            Replacements proposed;
            proposed[font.stream.getObjGen()] =
                stream_replacement(font.stream, deflate(bytes), "/FlateDecode");
            rename(font, candidate, changes, proposed);
            std::vector<FontMappingStream> streams;
            if (font.keep.size() < font.data->glyphs().size()) {
                FontMappingStream cidset;
                unsigned int last = 0;
                for (const auto& [name, glyph] : candidate.glyphs())
                    last = std::max(last, cid(name));
                std::string bits(last / 8 + 1, '\0');
                for (const auto& [name, glyph] : candidate.glyphs())
                    bits[cid(name) / 8] |= static_cast<char>(128 >> (cid(name) % 8));
                cidset.bytes = deflate(bits);
                for (const auto& owner : font.owners) {
                    auto descriptor = proposed.at(owner.descriptor.getObjGen()).value;
                    if (descriptor.hasKey("/CIDSet")) {
                        descriptor.removeKey("/CIDSet");
                        cidset.references.emplace_back(owner.descriptor.getObjGen(), "/CIDSet");
                    }
                }
                if (!cidset.references.empty())
                    streams.push_back(std::move(cidset));
            }
            if (auto saved = apply_font_plan(pdf, changes, proposed, std::move(streams))) {
                replaced(font);
                progress.bytes_saved += saved;
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
        for (std::size_t j = i + 1; j < fonts.size(); ++j) {
            auto& other = fonts[j];
            if (!other.error.empty() || other.merged || !other.can_merge ||
                !first.data->compatible(*other.data) ||
                first.data->glyphs().at(".notdef") != other.data->glyphs().at(".notdef"))
                continue;
            if (std::any_of(members.begin(), members.end(), [&](auto member) {
                    return fonts[member].data->glyphs() == other.data->glyphs() ||
                           fonts[member].data->has_shared_outlines(*other.data);
                }))
                members.push_back(j);
        }
        if (members.size() == 1)
            continue;
        try {
            std::vector<FontObject*> sources;
            std::vector<GlyphMap> mappings;
            std::map<unsigned int, FontObject::Glyph> glyphs{
                {0, first.data->glyphs().at(".notdef")}};
            for (auto index : members) {
                auto& font = fonts[index];
                sources.push_back(font.data.get());
                GlyphMap mapping{{".notdef", ".notdef"}};
                std::set<unsigned int> assigned{0};
                for (const auto& name : font.keep) {
                    if (name == ".notdef")
                        continue;
                    const auto& glyph = font.data->glyphs().at(name);
                    unsigned int target = cid(name);
                    auto match = glyphs.find(target);
                    if (match == glyphs.end() || assigned.contains(target) ||
                        match->second != glyph) {
                        match = std::find_if(glyphs.begin(), glyphs.end(), [&](const auto& item) {
                            return !assigned.contains(item.first) && item.second == glyph;
                        });
                        if (match != glyphs.end())
                            target = match->first;
                        else if (glyphs.contains(target) || assigned.contains(target)) {
                            target = 1;
                            while (target <= 65535 && glyphs.contains(target))
                                ++target;
                        }
                    }
                    if (!glyphs.contains(target)) {
                        if (target > 65535)
                            throw std::runtime_error("merged CID font exceeds 65536 glyphs");
                        glyphs.emplace(target, glyph);
                    }
                    assigned.insert(target);
                    mapping.emplace(name, cid_name(target));
                }
                mappings.push_back(std::move(mapping));
            }
            auto bytes = FontObject::cff(sources, nullptr, first.output_name, &mappings);
            FontObject candidate(bytes, false);
            for (std::size_t n = 0; n < members.size(); ++n)
                if (!candidate.preserves(*sources[n], fonts[members[n]].keep, &mappings[n]))
                    throw std::runtime_error("merged CID CFF changed glyphs or hints");
            Replacements proposed;
            std::vector<FontMappingStream> new_streams;
            proposed[first.stream.getObjGen()] =
                stream_replacement(first.stream, deflate(bytes), "/FlateDecode");
            FontMappingStream cidset;
            std::string bits(glyphs.rbegin()->first / 8 + 1, '\0');
            for (const auto& [bit, glyph] : glyphs)
                bits[bit / 8] |= static_cast<char>(128 >> (bit % 8));
            cidset.bytes = deflate(bits);
            for (std::size_t n = 0; n < members.size(); ++n) {
                auto& font = fonts[members[n]];
                const bool identity =
                    font.keep.size() == font.data->glyphs().size() &&
                    std::all_of(mappings[n].begin(), mappings[n].end(),
                                [](const auto& pair) { return pair.first == pair.second; });
                if (n)
                    proposed[font.stream.getObjGen()] = {Object::newNull(), {}, false};
                rename(font, candidate, changes, proposed);
                for (const auto& owner : font.owners) {
                    auto descriptor = proposed.at(owner.descriptor.getObjGen()).value;
                    descriptor.replaceKey("/FontFile3", first.stream);
                    if (descriptor.hasKey("/CIDSet")) {
                        descriptor.removeKey("/CIDSet");
                        cidset.references.emplace_back(owner.descriptor.getObjGen(), "/CIDSet");
                    }
                    if (identity)
                        continue;
                    bool vertical = false;
                    for (auto parent : owner.parents) {
                        const bool v = parent.getKey("/Encoding").isNameAndEquals("/Identity-V");
                        vertical |= v;
                        auto name = "/DPDFCompEncoding-" +
                                    std::to_string(parent.getObjGen().getObj()) + "-" +
                                    std::to_string(parent.getObjGen().getGen());
                        const auto base = name;
                        for (unsigned int suffix = 1; !cmap_names.insert(name).second; ++suffix)
                            name = base + "-" + std::to_string(suffix);
                        auto encoding =
                            cmap(mappings[n], owner.font.getKey("/CIDSystemInfo"), v, name);
                        encoding.references.emplace_back(parent.getObjGen(), "/Encoding");
                        new_streams.push_back(std::move(encoding));
                        proposed.at(parent.getObjGen()).value.removeKey("/Encoding");
                    }
                    remap_widths(proposed.at(owner.font.getObjGen()).value, mappings[n], vertical);
                }
            }
            if (!cidset.references.empty())
                new_streams.push_back(std::move(cidset));
            if (auto saved =
                    apply_font_plan(pdf, changes, std::move(proposed), std::move(new_streams))) {
                progress.bytes_saved += saved;
                for (auto index : members) {
                    replaced(fonts[index]);
                    fonts[index].merged = true;
                }
                report.fonts_merged += members.size() - 1;
                notify();
            }
        } catch (const std::exception& error) {
            report.notes.push_back("CID merge " + object_label(first.stream.getObjGen()) + ": " +
                                   error.what());
        }
    }
}
} // namespace dpdfcomp
