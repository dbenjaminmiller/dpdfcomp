#include "fonts.hpp"

#include "compression.hpp"
#include "font_program.hpp"
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{

struct Owner {
    Object font;
    Object descriptor;
};
struct Font {
    Object stream;
    bool type1 = false;
    bool replaced = false;
    std::vector<Owner> owners;
    std::string bytes;
    std::unique_ptr<FontProgram> program;
    std::set<std::string> keep;
    std::string output_name;
    std::unique_ptr<FontProgram> candidate;
    std::string compressed;
    std::string error;
};

using Encoding = std::array<std::string, 256>;
std::optional<Encoding> encoding(Object font, const FontProgram& program)
{
    auto definition = font.getKey("/Encoding");
    auto base = definition.isDictionary() ? definition.getKey("/BaseEncoding") : definition;
    Encoding result;
    if (base.isNull())
        result = program.encoding();
    else if (base.isNameAndEquals("/StandardEncoding"))
        result = standard_encoding();
    else
        return std::nullopt; // Keep every glyph for other predefined encodings.
    if (definition.isDictionary() && definition.hasKey("/Differences")) {
        auto differences = definition.getKey("/Differences");
        if (!differences.isArray())
            throw std::runtime_error("invalid font encoding differences");
        int code = -1;
        for (auto item : differences.getArrayAsVector()) {
            if (item.isInteger() && item.getIntValue() >= 0 && item.getIntValue() <= 255)
                code = static_cast<int>(item.getIntValue());
            else if (item.isName() && code >= 0 && code <= 255)
                result[code++] = item.getName().substr(1);
            else
                throw std::runtime_error("invalid font encoding differences");
        }
    }
    return result;
}

std::set<std::string> all_names(const FontProgram& program)
{
    std::set<std::string> names;
    for (const auto& [name, glyph] : program.glyphs()) {
        (void)glyph;
        names.insert(name);
    }
    return names;
}

bool preserves_mappings(const Font& source, const FontProgram& candidate)
{
    for (const auto& owner : source.owners) {
        auto before = encoding(owner.font, *source.program);
        auto after = encoding(owner.font, candidate);
        if (!before || !after) {
            // Unknown PDF encoding: require identical glyph coverage and built-in encoding.
            if (all_names(*source.program) != all_names(candidate) ||
                source.program->encoding() != candidate.encoding())
                return false;
            continue;
        }
        for (int code = 0; code < 256; ++code) {
            auto old_name =
                source.program->glyphs().contains((*before)[code]) ? (*before)[code] : ".notdef";
            auto new_name =
                candidate.glyphs().contains((*after)[code]) ? (*after)[code] : ".notdef";
            if (old_name != new_name)
                return false;
        }
    }
    return true;
}

Replacement font_stream(const Font& font, std::string compressed)
{
    auto replacement = stream_replacement(font.stream, std::move(compressed), "/FlateDecode");
    replacement.value.replaceKey("/Subtype", Object::newName("/Type1C"));
    for (const auto* key : {"/Length1", "/Length2", "/Length3"})
        replacement.value.removeKey(key);
    return replacement;
}

Object edited_dictionary(Object object, const Replacements& changes)
{
    auto prior = changes.find(object.getObjGen());
    auto value = prior == changes.end() ? object : prior->second.value;
    return value.shallowCopy();
}

void assign_font(const Font& source, Object destination, const std::string& name, bool rename,
                 const FontProgram& program, Replacements& changes)
{
    for (const auto& owner : source.owners) {
        auto descriptor = edited_dictionary(owner.descriptor, changes);
        descriptor.removeKey("/FontFile");
        descriptor.replaceKey("/FontFile3", destination);
        if (rename)
            descriptor.replaceKey("/FontName", Object::newName("/" + name));
        if (descriptor.hasKey("/CharSet")) {
            std::string charset;
            for (const auto& [glyph, data] : program.glyphs()) {
                (void)data;
                if (glyph != ".notdef")
                    charset += Object::newName("/" + glyph).unparse();
            }
            descriptor.replaceKey("/CharSet", Object::newString(charset));
        }
        changes[owner.descriptor.getObjGen()] = {descriptor, {}, false};
        if (rename) {
            auto font = edited_dictionary(owner.font, changes);
            font.replaceKey("/BaseFont", Object::newName("/" + name));
            changes[owner.font.getObjGen()] = {font, {}, false};
        }
    }
}

std::string family_name(std::string name)
{
    if (name.size() > 7 && name[6] == '+' &&
        std::all_of(name.begin(), name.begin() + 6, [](char c) { return c >= 'A' && c <= 'Z'; }))
        name.erase(0, 7);
    return name;
}

std::size_t apply_if_smaller(QPDF& pdf, const Replacements& proposed, Replacements& changes)
{
    std::size_t before = 0, after = 0;
    for (const auto& [id, replacement] : proposed) {
        auto previous = changes.find(id);
        before += previous == changes.end() ? stored_size(pdf.getObjectByObjGen(id))
                                            : stored_size(previous->second);
        after += stored_size(replacement);
    }
    if (after >= before)
        return 0;
    for (const auto& [id, replacement] : proposed)
        changes[id] = replacement;
    return before - after;
}

} // namespace

void optimize_fonts(QPDF& pdf, Replacements& changes, const Options& options, Report& report)
{
    if (!options.upgrade_version && effective_version(pdf) < PDFVersion(1, 2)) {
        report.notes.push_back("font optimization requires PDF 1.2; preserving the PDF version");
        return;
    }
    auto objects = pdf.getAllObjects();
    auto references = reference_counts(pdf);
    std::map<ObjectId, std::size_t> positions;
    std::vector<Font> fonts;
    for (auto object : objects) {
        if (!object.isDictionary() || !object.getKey("/Subtype").isNameAndEquals("/Type1"))
            continue;
        auto descriptor = object.getKey("/FontDescriptor");
        if (!descriptor.isIndirect() || !descriptor.isDictionary())
            continue;
        auto stream = descriptor.getKey("/FontFile");
        bool type1 = stream.isStream();
        if (!type1) {
            stream = descriptor.getKey("/FontFile3");
            if (!stream.isStream() ||
                !stream.getDict().getKey("/Subtype").isNameAndEquals("/Type1C"))
                continue;
        }
        if (type1 && !options.type1_to_cff)
            continue;
        auto [position, added] = positions.emplace(stream.getObjGen(), fonts.size());
        if (added) {
            Font font;
            font.stream = stream;
            font.type1 = type1;
            fonts.push_back(std::move(font));
        }
        fonts[position->second].owners.push_back({object, descriptor});
    }
    Progress progress{Progress::Kind::fonts};
    progress.total = fonts.size();
    auto notify = [&] {
        progress.compressed = report.fonts_replaced;
        if (options.progress)
            options.progress(progress);
    };
    auto mark_replaced = [&](Font& font) {
        if (!font.replaced) {
            font.replaced = true;
            ++report.fonts_replaced;
        }
    };
    notify();
    // A stream or descriptor shared with an unsupported consumer is left intact.
    for (auto& font : fonts) {
        std::map<ObjectId, std::size_t> descriptors;
        for (const auto& owner : font.owners)
            ++descriptors[owner.descriptor.getObjGen()];
        if (references[font.stream.getObjGen()] != descriptors.size())
            font.error = "font program has unsupported consumers";
        for (const auto& [id, count] : descriptors)
            if (references[id] != count)
                font.error = "font descriptor has unsupported consumers";
        if (font.error.empty()) {
            try {
                font.bytes = decoded_bytes(font.stream);
            } catch (const std::exception& e) {
                font.error = e.what();
            }
        }
    }
    // Parsing and rewriting font programs are independent. PDF objects stay on this thread.
    oneapi::tbb::parallel_for(std::size_t(0), fonts.size(), [&](std::size_t i) {
        auto& font = fonts[i];
        if (!font.error.empty())
            return;
        try {
            font.program = std::make_unique<FontProgram>(std::move(font.bytes), font.type1);
        } catch (const std::exception& e) {
            font.error = e.what();
        }
    });
    std::set<std::string> font_names;
    for (const auto& font : fonts)
        if (font.program)
            font_names.insert(font.program->name());
    for (auto& font : fonts) {
        if (!font.error.empty())
            continue;
        try {
            if (options.subset_fonts) {
                for (const auto& owner : font.owners) {
                    auto map = encoding(owner.font, *font.program);
                    if (map)
                        font.keep.insert(map->begin(), map->end());
                    else {
                        auto names = all_names(*font.program);
                        font.keep.insert(names.begin(), names.end());
                    }
                }
            } else
                font.keep = all_names(*font.program);
            font.keep = font.program->closure(std::move(font.keep));
            font.output_name = font.program->name();
            std::size_t retained = 0;
            for (const auto& [name, glyph] : font.program->glyphs()) {
                (void)glyph;
                retained += font.keep.contains(name);
            }
            if (retained < font.program->glyphs().size() &&
                family_name(font.output_name) == font.output_name) {
                // Deterministic, without colliding with existing embedded names.
                unsigned int seed = static_cast<unsigned int>(font.stream.getObjectID());
                std::string name;
                do {
                    if (seed >= 308915776U)
                        throw std::runtime_error("font ID exceeds subset tag range");
                    std::string tag(6, 'A');
                    auto id = seed++;
                    for (auto& letter : tag) {
                        letter = static_cast<char>('A' + id % 26);
                        id /= 26;
                    }
                    name = tag + "+" + font.output_name;
                } while (!font_names.insert(name).second);
                font.output_name = std::move(name);
            }
        } catch (const std::exception& e) {
            font.error = e.what();
        }
    }
    auto next = fonts.begin();
    auto threads = options.threads ? options.threads : oneapi::tbb::info::default_concurrency();
    oneapi::tbb::parallel_pipeline(
        std::max(1, threads * 2),
        oneapi::tbb::make_filter<void, Font*>(oneapi::tbb::filter_mode::serial_in_order,
                                              [&](oneapi::tbb::flow_control& flow) -> Font* {
                                                  if (next == fonts.end()) {
                                                      flow.stop();
                                                      return nullptr;
                                                  }
                                                  return &*next++;
                                              }) &
            oneapi::tbb::make_filter<Font*, Font*>(
                oneapi::tbb::filter_mode::parallel,
                [&](Font* font) {
                    if (font->error.empty()) {
                        try {
                            auto bytes = FontProgram::cff({font->program.get()}, &font->keep,
                                                          font->output_name);
                            font->compressed = deflate(bytes);
                            font->candidate =
                                std::make_unique<FontProgram>(std::move(bytes), false);
                            if (!font->candidate->preserves(*font->program, font->keep))
                                throw std::runtime_error(
                                    "CFF round-trip changed drawing, metrics, or hints");
                        } catch (const std::exception& e) {
                            font->candidate.reset();
                            font->compressed.clear();
                            font->error = e.what();
                        }
                    }
                    return font;
                }) &
            oneapi::tbb::make_filter<Font*, void>(
                oneapi::tbb::filter_mode::serial_in_order, [&](Font* font) {
                    // Only this stage accesses PDF objects during the font pipeline.
                    auto candidate = std::move(font->candidate);
                    if (font->error.empty() && !preserves_mappings(*font, *candidate))
                        font->error = "CFF round-trip changed character mapping";
                    if (!font->error.empty()) {
                        report.notes.push_back("font " + object_label(font->stream.getObjGen()) +
                                               ": " + font->error);
                    } else {
                        Replacements proposed;
                        proposed[font->stream.getObjGen()] =
                            font_stream(*font, std::move(font->compressed));
                        assign_font(*font, font->stream, candidate->name(),
                                    candidate->name() != font->program->name(), *candidate,
                                    proposed);
                        if (auto saved = apply_if_smaller(pdf, proposed, changes)) {
                            mark_replaced(*font);
                            progress.bytes_saved += saved;
                        }
                    }
                    ++progress.processed;
                    notify();
                }));
    std::set<std::size_t> merged;
    for (std::size_t i = 0; options.merge_fonts && i < fonts.size(); ++i) {
        auto& first = fonts[i];
        if (!first.error.empty() || merged.contains(i))
            continue;
        std::vector<FontProgram*> sources{first.program.get()};
        std::vector<std::size_t> members{i};
        auto keep = first.keep;
        for (std::size_t j = i + 1; j < fonts.size(); ++j) {
            auto& second = fonts[j];
            if (!second.error.empty() || merged.contains(j) ||
                !first.program->compatible(*second.program))
                continue;
            if (std::none_of(sources.begin(), sources.end(), [&](const auto* source) {
                    return source->has_shared_outlines(*second.program);
                }))
                continue;
            try {
                auto candidates = sources;
                candidates.push_back(second.program.get());
                auto candidate_keep = keep;
                candidate_keep.insert(second.keep.begin(), second.keep.end());
                auto bytes = FontProgram::cff(candidates, &candidate_keep, first.output_name);
                FontProgram candidate(bytes, false);
                bool preserves = candidate.preserves(*second.program, second.keep) &&
                                 preserves_mappings(second, candidate);
                for (auto member : members)
                    preserves &= candidate.preserves(*fonts[member].program, fonts[member].keep) &&
                                 preserves_mappings(fonts[member], candidate);
                if (!preserves)
                    continue;
                Replacements proposed;
                proposed[first.stream.getObjGen()] = font_stream(first, deflate(bytes));
                proposed[second.stream.getObjGen()] = {Object::newNull(), {}, false};
                assign_font(second, first.stream, candidate.name(), true, candidate, proposed);
                for (auto member : members)
                    assign_font(fonts[member], first.stream, candidate.name(), true, candidate,
                                proposed);
                auto saved = apply_if_smaller(pdf, proposed, changes);
                if (!saved)
                    continue;
                progress.bytes_saved += saved;
                mark_replaced(first);
                mark_replaced(second);
                sources = std::move(candidates);
                keep = std::move(candidate_keep);
                members.push_back(j);
                merged.insert(j);
                ++report.fonts_merged;
            } catch (const std::exception& e) {
                report.notes.push_back("merge " + object_label(first.stream.getObjGen()) + "/" +
                                       object_label(second.stream.getObjGen()) + ": " + e.what());
            }
            notify();
        }
    }
    progress.complete = true;
    notify();
}

} // namespace dpdfcomp
