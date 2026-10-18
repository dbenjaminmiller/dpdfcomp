#include "truetype_font_object.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace dpdfcomp
{
namespace
{
void require(bool value)
{
    if (!value)
        throw std::runtime_error("unsupported or invalid TrueType font");
}
std::uint32_t read(std::string_view bytes, std::size_t offset, std::size_t count)
{
    require(offset <= bytes.size() && count <= bytes.size() - offset);
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}
void put(std::string& bytes, std::size_t offset, std::uint32_t value, std::size_t count)
{
    require(offset <= bytes.size() && count <= bytes.size() - offset);
    for (std::size_t i = count; i; --i) {
        bytes[offset + i - 1] = static_cast<char>(value);
        value >>= 8;
    }
}
void append(std::string& bytes, std::uint32_t value, std::size_t count)
{
    auto offset = bytes.size();
    bytes.resize(offset + count);
    put(bytes, offset, value, count);
}
std::uint32_t checksum(std::string_view bytes)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const auto count = std::min<std::size_t>(4, bytes.size() - i);
        sum += read(bytes, i, count) << (8 * (4 - count));
    }
    return sum;
}
std::string serialize(std::map<std::string, std::string> tables)
{
    require(tables.contains("head") && tables.size() <= 4095);
    put(tables.at("head"), 8, 0, 4);
    std::string output(12 + 16 * tables.size(), '\0');
    put(output, 0, 0x00010000, 4);
    put(output, 4, tables.size(), 2);
    unsigned int power = 1, exponent = 0;
    while (power * 2 <= tables.size()) {
        power *= 2;
        ++exponent;
    }
    put(output, 6, power * 16, 2);
    put(output, 8, exponent, 2);
    put(output, 10, tables.size() * 16 - power * 16, 2);
    std::size_t record = 12, head = 0;
    for (const auto& [tag, bytes] : tables) {
        require(tag.size() == 4 && output.size() <= std::numeric_limits<std::uint32_t>::max() &&
                bytes.size() <= std::numeric_limits<std::uint32_t>::max());
        output.replace(record, 4, tag);
        put(output, record + 4, checksum(bytes), 4);
        put(output, record + 8, output.size(), 4);
        put(output, record + 12, bytes.size(), 4);
        if (tag == "head")
            head = output.size();
        output += bytes;
        while (output.size() % 4)
            output.push_back('\0');
        record += 16;
    }
    put(output, head + 8, 0xB1B0AFBAu - checksum(output), 4);
    return output;
}
std::string normalized(std::string tag, std::string bytes)
{
    if (tag == "head") {
        put(bytes, 8, 0, 4);  // checksum
        put(bytes, 50, 0, 2); // loca representation
        for (std::size_t i = 20; i < 36; ++i)
            bytes.at(i) = 0; // creation/modification timestamps
    } else if (tag == "hhea" || tag == "vhea") {
        for (std::size_t i = 10; i < 18; ++i)
            bytes.at(i) = 0; // extrema over the subset's glyphs
        put(bytes, 34, 0, 2);
    } else if (tag == "maxp")
        bytes.resize(4);
    else if (tag == "post")
        bytes = bytes.substr(4, 28); // names and their indexing are rebuilt for CID use
    return bytes;
}
std::set<std::string> license_records(std::string_view table)
{
    std::set<std::string> result;
    require(read(table, 0, 2) <= 1);
    const auto count = read(table, 2, 2), strings = read(table, 4, 2);
    for (unsigned int i = 0; i < count; ++i) {
        const auto record = 6 + i * 12;
        auto id = read(table, record + 6, 2);
        auto length = read(table, record + 8, 2), offset = read(table, record + 10, 2);
        require(strings + offset <= table.size() && length <= table.size() - strings - offset);
        if (id == 0 || id == 7 || id == 13 || id == 14)
            result.insert(std::string(table.substr(record, 8)) +
                          std::string(table.substr(strings + offset, length)));
    }
    return result;
}

void set_name(std::map<std::string, std::string>& tables, const std::string& name)
{
    if (name.empty())
        return;
    require(
        std::all_of(name.begin(), name.end(), [](unsigned char c) { return c > 32 && c < 127; }));
    std::string wide;
    for (auto c : name) {
        wide.push_back(0);
        wide.push_back(c);
    }
    if (!tables.contains("name")) {
        std::string table(18, '\0');
        put(table, 2, 1, 2);
        put(table, 4, 18, 2);
        put(table, 6, 3, 2);
        put(table, 8, 1, 2);
        put(table, 10, 0x409, 2);
        put(table, 12, 6, 2);
        put(table, 14, wide.size(), 2);
        tables["name"] = table + wide;
        return;
    }
    auto& table = tables.at("name");
    const auto count = read(table, 2, 2), strings = read(table, 4, 2);
    bool replaced = false;
    for (unsigned int i = 0; i < count; ++i) {
        const auto record = 6 + i * 12;
        if (read(table, record + 6, 2) != 6)
            continue;
        auto platform = read(table, record, 2);
        const auto& text = platform == 0 || platform == 3 ? wide : name;
        require(strings <= table.size() && table.size() - strings <= 65535 && text.size() <= 65535);
        put(table, record + 8, text.size(), 2);
        put(table, record + 10, table.size() - strings, 2);
        table += text;
        replaced = true;
    }
    require(replaced);
}

std::string character_map(const std::map<unsigned int, unsigned int>& mapping)
{
    std::vector<std::pair<unsigned int, unsigned int>> bmp;
    for (auto entry : mapping)
        if (entry.first < 65535)
            bmp.push_back(entry);
    require(bmp.size() < 8190);
    bmp.emplace_back(65535, 0);
    const auto count = bmp.size();
    std::string format4(16 + 8 * count, '\0');
    put(format4, 0, 4, 2);
    put(format4, 2, format4.size(), 2);
    put(format4, 6, count * 2, 2);
    unsigned int power = 1, exponent = 0;
    while (power * 2 <= count) {
        power *= 2;
        ++exponent;
    }
    put(format4, 8, power * 2, 2);
    put(format4, 10, exponent, 2);
    put(format4, 12, count * 2 - power * 2, 2);
    for (std::size_t i = 0; i < count; ++i) {
        put(format4, 14 + i * 2, bmp[i].first, 2);
        put(format4, 16 + count * 2 + i * 2, bmp[i].first, 2);
        put(format4, 16 + count * 4 + i * 2, (bmp[i].second - bmp[i].first) & 65535, 2);
    }
    std::string format12(16, '\0');
    put(format12, 0, 12, 2);
    put(format12, 12, mapping.size(), 4);
    for (auto [code, glyph] : mapping) {
        append(format12, code, 4);
        append(format12, code, 4);
        append(format12, glyph, 4);
    }
    put(format12, 4, format12.size(), 4);
    std::string output(20, '\0');
    put(output, 2, 2, 2);
    put(output, 4, 3, 2);
    put(output, 6, 1, 2);
    put(output, 8, 20, 4);
    put(output, 12, 3, 2);
    put(output, 14, 10, 2);
    put(output, 16, 20 + format4.size(), 4);
    return output + format4 + format12;
}
} // namespace

TrueTypeFontObject::TrueTypeFontObject(std::string bytes)
{
    require(read(bytes, 0, 4) == 0x00010000);
    const auto count = read(bytes, 4, 2);
    for (unsigned int i = 0; i < count; ++i) {
        const auto record = 12 + 16 * i;
        auto offset = read(bytes, record + 8, 4), length = read(bytes, record + 12, 4);
        require(offset <= bytes.size() && length <= bytes.size() - offset);
        require(tables_.emplace(bytes.substr(record, 4), bytes.substr(offset, length)).second);
    }
    for (auto tag : {"head", "hhea", "maxp", "hmtx", "loca", "glyf"})
        require(tables_.contains(tag));
    // These formats need additional closure/variation or bitmap semantics.
    for (auto tag :
         {"fvar", "gvar", "CFF ", "CFF2", "COLR", "CBDT", "EBDT", "sbix", "SVG ", "gcid"})
        require(!tables_.contains(tag));
    require(tables_.at("head").size() == 54 && read(tables_.at("head"), 12, 4) == 0x5F0F3CF5);
    require(tables_.at("hhea").size() == 36 && tables_.at("maxp").size() == 32);
    if (tables_.contains("vhea"))
        require(tables_.at("vhea").size() == 36);
    if (tables_.contains("post"))
        require(tables_.at("post").size() >= 32);
    if (tables_.contains("name"))
        (void)license_records(tables_.at("name"));
    const auto glyph_count = read(tables_.at("maxp"), 4, 2);
    require(glyph_count > 0 && read(tables_.at("maxp"), 0, 4) == 0x00010000);
    auto format = read(tables_.at("head"), 50, 2);
    require(format <= 1);
    const unsigned int loca_width = format ? 4 : 2;
    require(tables_.at("loca").size() >= (glyph_count + 1) * loca_width);
    auto metric = [&](unsigned int glyph, const char* header, const char* table) {
        if (!tables_.contains(header) && !tables_.contains(table))
            return std::string{};
        require(tables_.contains(header) && tables_.contains(table));
        const auto& bytes = tables_.at(table);
        const auto n = read(tables_.at(header), 34, 2);
        require(n && n <= glyph_count && bytes.size() >= n * 4 + (glyph_count - n) * 2);
        std::string value;
        append(value, read(bytes, (glyph < n ? glyph : n - 1) * 4, 2), 2);
        append(value, read(bytes, glyph < n ? glyph * 4 + 2 : n * 4 + (glyph - n) * 2, 2), 2);
        return value;
    };
    for (unsigned int i = 0; i < glyph_count; ++i) {
        const auto start = read(tables_.at("loca"), i * loca_width, loca_width) * (format ? 1 : 2);
        const auto end =
            read(tables_.at("loca"), (i + 1) * loca_width, loca_width) * (format ? 1 : 2);
        require(start <= end && end <= tables_.at("glyf").size());
        Glyph glyph{tables_.at("glyf").substr(start, end - start),
                    metric(i, "hhea", "hmtx"),
                    metric(i, "vhea", "vmtx"),
                    {}};
        if (!glyph.bytes.empty()) {
            require(glyph.bytes.size() >= 10);
            if (read(glyph.bytes, 0, 2) >= 0x8000) {
                require(read(glyph.bytes, 0, 2) == 0xffff);
                unsigned int flags;
                std::size_t position = 10;
                do {
                    flags = read(glyph.bytes, position, 2);
                    require(read(glyph.bytes, position + 2, 2) < glyph_count);
                    glyph.component_offsets.push_back(position + 2);
                    position += 4 + ((flags & 1) ? 4 : 2);
                    const auto scale = flags & (8 | 64 | 128);
                    require(!scale || scale == 8 || scale == 64 || scale == 128);
                    position += scale == 8 ? 2 : scale == 64 ? 4 : scale == 128 ? 8 : 0;
                    require(position <= glyph.bytes.size());
                } while (flags & 32);
                if (flags & 256) {
                    const auto instructions = read(glyph.bytes, position, 2);
                    require(position + 2 + instructions <= glyph.bytes.size());
                }
            }
        }
        glyphs_.push_back(std::move(glyph));
    }
    FT_Library library = nullptr;
    require(!FT_Init_FreeType(&library));
    std::unique_ptr<FT_LibraryRec_, decltype(&FT_Done_FreeType)> owner(library, FT_Done_FreeType);
    FT_Face face = nullptr;
    require(bytes.size() <= std::size_t(std::numeric_limits<FT_Long>::max()) &&
            !FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(bytes.data()),
                                bytes.size(), 0, &face));
    std::unique_ptr<FT_FaceRec_, decltype(&FT_Done_Face)> font(face, FT_Done_Face);
    require(face->num_faces == 1 && !FT_HAS_MULTIPLE_MASTERS(face));
    for (unsigned int i = 0; i < glyph_count; ++i)
        require(!FT_Load_Glyph(face, i, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP));
    unicode_available_ = FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0;
    if (unicode_available_) {
        FT_UInt glyph;
        auto code = FT_Get_First_Char(face, &glyph);
        while (glyph) {
            require(code <= 0x10ffff);
            unicode_[code] = glyph;
            code = FT_Get_Next_Char(face, code, &glyph);
        }
    }
    names_available_ = FT_HAS_GLYPH_NAMES(face);
    if (names_available_)
        for (unsigned int i = 0; i < glyph_count; ++i) {
            char name[256]{};
            require(!FT_Get_Glyph_Name(face, i, name, sizeof(name)));
            require(names_.emplace(name, i).second);
        }
}

unsigned int TrueTypeFontObject::glyph_count() const
{
    return glyphs_.size();
}

std::optional<unsigned int> TrueTypeFontObject::named_glyph(const std::string& name) const
{
    if (name == ".notdef")
        return 0;
    if (!names_available_)
        return std::nullopt;
    auto found = names_.find(name);
    return found == names_.end() ? 0 : found->second;
}

std::optional<unsigned int> TrueTypeFontObject::unicode_glyph(unsigned int codepoint) const
{
    if (!unicode_available_)
        return std::nullopt;
    auto found = unicode_.find(codepoint);
    return found == unicode_.end() ? 0 : found->second;
}

bool TrueTypeFontObject::compatible(const TrueTypeFontObject& other) const
{
    const std::set<std::string> supported{"head", "hhea", "vhea", "maxp", "hmtx", "vmtx", "glyf",
                                          "loca", "cmap", "name", "post", "OS/2", "cvt ", "fpgm",
                                          "prep", "gasp", "DSIG", "GSUB", "GPOS", "GDEF", "kern"};
    const std::set<std::string> rebuilt{"glyf", "loca", "hmtx", "vmtx", "cmap", "name", "DSIG"};
    for (const auto* font : {this, &other})
        for (const auto& [tag, value] : font->tables_)
            if (!supported.contains(tag) ||
                (!rebuilt.contains(tag) &&
                 (!tables_.contains(tag) || !other.tables_.contains(tag) ||
                  normalized(tag, tables_.at(tag)) != normalized(tag, other.tables_.at(tag)))))
                return false;
    if (tables_.contains("vmtx") != other.tables_.contains("vmtx"))
        return false;
    const auto names = [](const auto& tables) {
        return tables.contains("name") ? license_records(tables.at("name"))
                                       : std::set<std::string>{};
    };
    return names(tables_) == names(other.tables_);
}

std::set<unsigned int> TrueTypeFontObject::closure(std::set<unsigned int> keep) const
{
    keep.insert(0);
    std::vector<unsigned int> pending(keep.begin(), keep.end());
    while (!pending.empty()) {
        auto index = pending.back();
        pending.pop_back();
        require(index < glyphs_.size());
        for (auto offset : glyphs_[index].component_offsets) {
            auto component = read(glyphs_[index].bytes, offset, 2);
            if (keep.insert(component).second)
                pending.push_back(component);
        }
    }
    return keep;
}

std::string TrueTypeFontObject::subset(const std::set<unsigned int>& selected,
                                       const std::string& name) const
{
    auto keep = closure(selected);
    auto tables = tables_;
    std::string glyphs, loca;
    for (unsigned int i = 0; i < glyphs_.size(); ++i) {
        append(loca, glyphs.size(), 4);
        if (keep.contains(i))
            glyphs += glyphs_[i].bytes;
        while (glyphs.size() % 4)
            glyphs.push_back('\0');
    }
    append(loca, glyphs.size(), 4);
    tables["glyf"] = std::move(glyphs);
    tables["loca"] = std::move(loca);
    put(tables.at("head"), 50, 1, 2);
    tables.erase("DSIG");
    set_name(tables, name);
    return serialize(std::move(tables));
}

TrueTypeFontObject::Result
TrueTypeFontObject::merge(const std::vector<const TrueTypeFontObject*>& sources,
                          const std::vector<std::set<unsigned int>>& keep, bool cid,
                          const std::string& name)
{
    require(!sources.empty() && keep.size() == sources.size());
    auto tables = sources.front()->tables_;
    std::vector<Glyph> glyphs;
    std::map<std::string, unsigned int> identities;
    std::map<unsigned int, unsigned int> characters;
    std::map<std::string, unsigned int> names;
    Result result;
    for (std::size_t n = 0; n < sources.size(); ++n) {
        const auto& source = *sources[n];
        require(sources.front()->compatible(source));
        GlyphMap mapping;
        std::set<unsigned int> active;
        std::function<unsigned int(unsigned int)> add = [&](unsigned int old) -> unsigned int {
            if (auto found = mapping.find(old); found != mapping.end())
                return found->second;
            require(old < source.glyphs_.size() && active.insert(old).second &&
                    active.size() <= 64);
            auto glyph = source.glyphs_[old];
            // Reserve GID zero before visiting any .notdef composite components.
            const bool first_notdef = old == 0 && glyphs.empty();
            if (first_notdef)
                glyphs.push_back({});
            for (auto offset : glyph.component_offsets)
                put(glyph.bytes, offset, add(read(glyph.bytes, offset, 2)), 2);
            while (!glyph.bytes.empty() && glyph.bytes.size() % 4)
                glyph.bytes.push_back('\0');
            std::string key = glyph.horizontal + glyph.vertical + glyph.bytes;
            if (!cid && source.names_available_) {
                auto named = std::find_if(source.names_.begin(), source.names_.end(),
                                          [&](const auto& item) { return item.second == old; });
                require(named != source.names_.end());
                key += '\0' + named->first;
            }
            auto found = identities.find(key);
            unsigned int target;
            if (first_notdef) {
                target = 0;
                glyphs[0] = std::move(glyph);
                identities.emplace(key, 0);
            } else if (found != identities.end())
                target = found->second;
            else {
                require(old != 0 && glyphs.size() < 65535);
                target = glyphs.size();
                glyphs.push_back(std::move(glyph));
                identities.emplace(key, target);
            }
            require(old != 0 || target == 0);
            mapping.emplace(old, target);
            active.erase(old);
            return target;
        };
        add(0);
        for (auto index : source.closure(keep[n]))
            add(index);
        result.mappings.push_back(std::move(mapping));
        if (!cid) {
            const auto& remap = result.mappings.back();
            for (const auto& [code, glyph] : source.unicode_)
                if (auto found = remap.find(glyph); found != remap.end()) {
                    auto [prior, inserted] = characters.emplace(code, found->second);
                    require(inserted || prior->second == found->second);
                }
            for (const auto& [name, glyph] : source.names_)
                if (auto found = remap.find(glyph); found != remap.end()) {
                    auto [prior, inserted] = names.emplace(name, found->second);
                    require(inserted || prior->second == found->second);
                }
        }
        for (std::size_t i = 6; i < 32; i += 2)
            put(tables.at("maxp"), i,
                std::max(read(tables.at("maxp"), i, 2), read(source.tables_.at("maxp"), i, 2)), 2);
        for (auto header : {"hhea", "vhea"}) {
            if (!tables.contains(header))
                continue;
            auto& destination = tables.at(header);
            const auto& original = source.tables_.at(header);
            put(destination, 10, std::max(read(destination, 10, 2), read(original, 10, 2)), 2);
            for (std::size_t offset : {12, 14, 16}) {
                auto a = static_cast<std::int16_t>(read(destination, offset, 2));
                auto b = static_cast<std::int16_t>(read(original, offset, 2));
                put(destination, offset,
                    static_cast<std::uint16_t>(offset == 16 ? std::max(a, b) : std::min(a, b)), 2);
            }
        }
    }
    std::string data, loca, horizontal, vertical;
    for (const auto& glyph : glyphs) {
        append(loca, data.size(), 4);
        data += glyph.bytes;
        horizontal += glyph.horizontal;
        vertical += glyph.vertical;
    }
    append(loca, data.size(), 4);
    tables["glyf"] = std::move(data);
    tables["loca"] = std::move(loca);
    tables["hmtx"] = std::move(horizontal);
    put(tables.at("head"), 50, 1, 2);
    put(tables.at("hhea"), 34, glyphs.size(), 2);
    if (!vertical.empty()) {
        tables["vmtx"] = std::move(vertical);
        put(tables.at("vhea"), 34, glyphs.size(), 2);
    }
    put(tables.at("maxp"), 4, glyphs.size(), 2);
    // CID fonts address glyphs through PDF CIDToGIDMap, not Unicode or names.
    tables.erase("cmap");
    if (tables.contains("post")) {
        tables["post"].resize(32);
        put(tables.at("post"), 0, 0x00030000, 4);
    }
    if (!cid) {
        tables["cmap"] = character_map(characters);
        if (!names.empty()) {
            auto& post = tables["post"];
            post.resize(32);
            put(post, 0, 0x00020000, 4);
            append(post, glyphs.size(), 2);
            std::vector<std::string> ordered(glyphs.size());
            for (const auto& [name, glyph] : names) {
                require(ordered[glyph].empty() || ordered[glyph] == name);
                ordered[glyph] = name;
            }
            require(glyphs.size() <= 65535 - 258);
            for (std::size_t i = 0; i < glyphs.size(); ++i)
                append(post, i + 258, 2);
            for (const auto& name : ordered) {
                require(!name.empty() && name.size() <= 255);
                post.push_back(static_cast<char>(name.size()));
                post += name;
            }
        }
    }
    // Tables with glyph-index references cannot be retained after renumbering.
    for (auto tag : {"DSIG", "GSUB", "GPOS", "GDEF", "kern", "BASE", "JSTF", "MATH", "LTSH", "hdmx",
                     "VDMX", "VORG"})
        tables.erase(tag);
    set_name(tables, name);
    result.bytes = serialize(std::move(tables));
    return result;
}

bool TrueTypeFontObject::preserves(const TrueTypeFontObject& source,
                                   const std::set<unsigned int>& keep,
                                   const GlyphMap& mapping) const
{
    for (auto old : source.closure(keep)) {
        auto found = mapping.find(old);
        if (found == mapping.end() || found->second >= glyphs_.size())
            return false;
        auto expected = source.glyphs_[old];
        const auto& actual = glyphs_[found->second];
        for (auto offset : expected.component_offsets) {
            auto component = mapping.find(read(expected.bytes, offset, 2));
            if (component == mapping.end())
                return false;
            put(expected.bytes, offset, component->second, 2);
        }
        while (expected.bytes.size() % 4)
            expected.bytes.push_back('\0');
        auto actual_bytes = actual.bytes;
        while (actual_bytes.size() % 4)
            actual_bytes.push_back('\0');
        if (expected.bytes != actual_bytes || expected.horizontal != actual.horizontal ||
            expected.vertical != actual.vertical)
            return false;
    }
    return true;
}
} // namespace dpdfcomp
