#include "font_program.hpp"

#include <absfont.h>
#include <cffread_abs.h>
#include <cffwrite.h>
#include <ft2build.h>
#include <t1read.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dpdfcomp
{
namespace
{

// AFDKO reports failures through return codes. Keep its optional diagnostics
// inside the adapter; the optimizer reports copied fonts through its callbacks.
class FontLogger final : public slogger {
public:
    void msg(int, const char*) override {}
    void vlog(int, const char*, va_list) override {}
    int set_context(const char*, int, const char*) override { return log_context_new; }
    int clear_context(const char*) override { return log_context_new; }
};

std::shared_ptr<slogger> font_logger()
{
    static const auto logger = std::make_shared<FontLogger>();
    return logger;
}

// These adapters implement AFDKO's I/O interface using ordinary C++ strings.
// Allocation policy stays with the C/C++ runtime and the libraries themselves.
struct Stream {
    std::string bytes;
    std::size_t position = 0;
    bool error = false;
};
struct IO {
    std::string source;
    std::map<int, Stream> streams;
    ctlMemoryCallbacks memory{};
    ctlStreamCallbacks callbacks{};

    explicit IO(std::string bytes = {}) : source(std::move(bytes))
    {
        memory.manage = [](ctlMemoryCallbacks*, void* old, std::size_t size) -> void* {
            if (!size) {
                std::free(old);
                return nullptr;
            }
            return std::realloc(old, size);
        };
        callbacks.direct_ctx = this;
        callbacks.open = [](ctlStreamCallbacks* cb, int id, std::size_t) -> void* {
            auto& io = *static_cast<IO*>(cb->direct_ctx);
            auto& stream = io.streams[id];
            stream = {};
            if (id == T1R_SRC_STREAM_ID || id == CFR_SRC_STREAM_ID)
                stream.bytes = io.source;
            return &stream;
        };
        callbacks.seek = [](ctlStreamCallbacks*, void* handle, long offset) {
            if (offset < 0)
                return -1;
            static_cast<Stream*>(handle)->position = static_cast<std::size_t>(offset);
            return 0;
        };
        callbacks.tell = [](ctlStreamCallbacks*, void* handle) -> long {
            auto position = static_cast<Stream*>(handle)->position;
            return position > std::size_t(std::numeric_limits<long>::max())
                       ? -1
                       : static_cast<long>(position);
        };
        callbacks.read = [](ctlStreamCallbacks*, void* handle, char** bytes) -> std::size_t {
            auto& stream = *static_cast<Stream*>(handle);
            if (stream.position >= stream.bytes.size())
                return 0;
            // AFDKO may keep a pointer until the next read. std::string owns it.
            auto length = stream.bytes.size() - stream.position;
            *bytes = stream.bytes.data() + stream.position;
            stream.position += length;
            return length;
        };
        callbacks.write = [](ctlStreamCallbacks*, void* handle, std::size_t count, const char* bytes) {
            auto& stream = *static_cast<Stream*>(handle);
            if (stream.position > stream.bytes.max_size() ||
                count > stream.bytes.max_size() - stream.position) {
                stream.error = true;
                return std::size_t(0);
            }
            stream.bytes.resize(std::max(stream.bytes.size(), stream.position + count));
            if (count)
                std::memcpy(stream.bytes.data() + stream.position, bytes, count);
            stream.position += count;
            return count;
        };
        callbacks.status = [](ctlStreamCallbacks*, void* handle) -> int {
            auto& stream = *static_cast<Stream*>(handle);
            return stream.error                             ? CTL_STREAM_ERROR
                   : stream.position >= stream.bytes.size() ? CTL_STREAM_END
                                                            : CTL_STREAM_OK;
        };
        callbacks.close = [](ctlStreamCallbacks*, void*) { return 0; };
    }
};

void check(int code, const char* (*message)(int))
{
    if (code)
        throw std::runtime_error(message(code));
}
const char* t1_message(int code)
{
    return t1rErrStr(code);
}
const char* cff_message(int code)
{
    return cfrErrStr(code);
}
const char* writer_message(int code)
{
    return cfwErrStr(code);
}

} // namespace

const std::array<std::string, 256>& standard_encoding()
{
    static const char* names[] = {
#include "../third_party/afdko/resource/stdstr1.h"
    };
    static const int sids[] = {
#include "../third_party/afdko/resource/stdenc1.h"
    };
    static const auto encoding = [] {
        std::array<std::string, 256> result;
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = names[sids[i]];
        return result;
    }();
    return encoding;
}

struct FontProgram::Impl {
    IO io;
    t1rCtx t1 = nullptr;
    cfrCtx cfr = nullptr;
    abfTopDict* top = nullptr;
    bool begun = false;
    std::map<std::string, Glyph> glyphs;
    std::array<std::string, 256> encoding;
    Glyph* current = nullptr;
    bool unsupported = false;

    explicit Impl(std::string data) : io(std::move(data)) { encoding.fill(".notdef"); }
    ~Impl()
    {
        if (t1) {
            if (begun)
                t1rEndFont(t1);
            t1rFree(t1);
        }
        if (cfr) {
            if (begun)
                cfrEndFont(cfr);
            cfrFree(cfr);
        }
    }
    void iterate(abfGlyphCallbacks& callbacks)
    {
        if (t1) {
            t1rResetGlyphs(t1);
            check(t1rIterateGlyphs(t1, &callbacks), t1_message);
        } else {
            cfrResetGlyphs(cfr);
            check(cfrIterateGlyphs(cfr, &callbacks), cff_message);
        }
    }
    static Impl& self(abfGlyphCallbacks* cb) { return *static_cast<Impl*>(cb->direct_ctx); }
    static void draw(abfGlyphCallbacks* cb, std::initializer_list<float> values)
    {
        auto& output = self(cb).current->drawing;
        output.insert(output.end(), values);
    }
    static void hint(abfGlyphCallbacks* cb, std::initializer_list<float> values)
    {
        auto& output = self(cb).current->hints;
        output.push_back(static_cast<float>(self(cb).current->drawing.size()));
        output.insert(output.end(), values);
    }

    void read(bool type1)
    {
        // Require an independent font engine to accept the original program.
        // A permissive parser must not turn an originally invalid font into a
        // visible one, changing a reader's missing-font fallback.
        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library))
            throw std::runtime_error("cannot initialize FreeType");
        std::unique_ptr<FT_LibraryRec_, decltype(&FT_Done_FreeType)> ft(library, FT_Done_FreeType);
        FT_Face face = nullptr;
        if (io.source.size() > std::size_t(std::numeric_limits<FT_Long>::max()) ||
            FT_New_Memory_Face(ft.get(), reinterpret_cast<const FT_Byte*>(io.source.data()),
                               static_cast<FT_Long>(io.source.size()), 0, &face))
            throw std::runtime_error("FreeType rejected the font program");
        bool unsupported_face = face->num_faces != 1 || FT_HAS_MULTIPLE_MASTERS(face);
        FT_Done_Face(face);
        if (unsupported_face)
            throw std::runtime_error("font collections and variable fonts are preserved");
        if (type1) {
            t1 = t1rNew(&io.memory, &io.callbacks, T1R_CHECK_ARGS, font_logger());
            if (!t1)
                throw std::runtime_error("cannot initialize Type 1 reader");
            check(t1rBegFont(t1, 0, 0, &top, nullptr), t1_message);
        } else {
            cfr = cfrNew(&io.memory, &io.callbacks, CFR_CHECK_ARGS, font_logger());
            if (!cfr)
                throw std::runtime_error("cannot initialize CFF reader");
            check(cfrBegFont(cfr, 0, 0, 0, &top, nullptr), cff_message);
        }
        begun = true;
        if (top->sup.flags || top->FDArray.cnt != 1 || top->varStore ||
            top->FDArray.array[0].Private.numRegions > 0)
            throw std::runtime_error("only static, name-keyed fonts are supported");
        // The CFF reader duplicates static values into its variation-only
        // fields; the Type 1 reader leaves those fields zero. They have no
        // meaning in the static fonts accepted here.
        auto& blends = top->FDArray.array[0].Private.blendValues;
        std::memset(&blends, 0, sizeof(blends));
        if (t1) {
            long masters = 0;
            t1rGetWV(t1, &masters);
            if (masters)
                throw std::runtime_error("multiple-master fonts are preserved");
        }
        abfGlyphCallbacks cb{};
        cb.direct_ctx = this;
        cb.beg = [](abfGlyphCallbacks* cb, abfGlyphInfo* info) -> int {
            cb->info = info;
            auto& reader = self(cb);
            if (!info->gname.ptr || (info->flags & (ABF_GLYPH_CID | ABF_GLYPH_UNICODE))) {
                reader.unsupported = true;
                return ABF_SKIP_RET;
            }
            auto [it, added] = reader.glyphs.emplace(info->gname.ptr, Glyph{});
            if (!added) {
                reader.unsupported = true;
                return ABF_SKIP_RET;
            }
            reader.current = &it->second;
            for (auto* entry = &info->encoding; entry; entry = entry->next)
                if (entry->code < 256)
                    reader.encoding[entry->code] = info->gname.ptr;
            return ABF_CONT_RET;
        };
        cb.width = [](abfGlyphCallbacks* cb, float width) { self(cb).current->width = width; };
        cb.move = [](abfGlyphCallbacks* cb, float x, float y) { draw(cb, {1, x, y}); };
        cb.line = [](abfGlyphCallbacks* cb, float x, float y) { draw(cb, {2, x, y}); };
        cb.curve = [](abfGlyphCallbacks* cb, float x1, float y1, float x2, float y2, float x3,
                      float y3) { draw(cb, {3, x1, y1, x2, y2, x3, y3}); };
        cb.stem = [](abfGlyphCallbacks* cb, int flags, float a, float b) {
            hint(cb, {1, static_cast<float>(flags), a, b});
        };
        cb.flex = [](abfGlyphCallbacks* cb, float depth, float x1, float y1, float x2, float y2,
                     float x3, float y3, float x4, float y4, float x5, float y5, float x6,
                     float y6) {
            draw(cb, {3, x1, y1, x2, y2, x3, y3, 3, x4, y4, x5, y5, x6, y6});
            hint(cb, {2, depth, x1, y1, x2, y2, x3, y3, x4, y4, x5, y5, x6, y6});
        };
        cb.genop = [](abfGlyphCallbacks* cb, int, float*, int) { self(cb).unsupported = true; };
        cb.seac = [](abfGlyphCallbacks* cb, float x, float y, int base, int accent) {
            if (base < 0 || base > 255 || accent < 0 || accent > 255) {
                self(cb).unsupported = true;
                return;
            }
            draw(cb, {4, x, y, static_cast<float>(base), static_cast<float>(accent)});
            self(cb).current->dependencies.insert(standard_encoding()[base]);
            self(cb).current->dependencies.insert(standard_encoding()[accent]);
        };
        cb.end = [](abfGlyphCallbacks*) {};
        iterate(cb);
        if (unsupported || !glyphs.contains(".notdef"))
            throw std::runtime_error("font contains unsupported glyph operations");
        for (const auto& [name, glyph] : glyphs) {
            (void)name;
            auto finite = [](float value) { return std::isfinite(value); };
            if (!finite(glyph.width) ||
                !std::all_of(glyph.drawing.begin(), glyph.drawing.end(), finite) ||
                !std::all_of(glyph.hints.begin(), glyph.hints.end(), finite))
                throw std::runtime_error("font contains nonfinite glyph coordinates or metrics");
        }
    }
};

FontProgram::FontProgram(std::string data, bool type1)
    : impl_(std::make_unique<Impl>(std::move(data)))
{
    impl_->read(type1);
}
FontProgram::~FontProgram() = default;
const std::map<std::string, FontProgram::Glyph>& FontProgram::glyphs() const
{
    return impl_->glyphs;
}
const std::array<std::string, 256>& FontProgram::encoding() const
{
    return impl_->encoding;
}
std::string FontProgram::name() const
{
    const auto* name = impl_->top->FDArray.array[0].FontName.ptr;
    return name ? name : "";
}

bool FontProgram::compatible(const FontProgram& other) const
{
    auto* a = impl_->top;
    auto* b = other.impl_->top;
    auto same_string = [](abfString x, abfString y) {
        return std::string_view(x.ptr ? x.ptr : "") == std::string_view(y.ptr ? y.ptr : "");
    };
    return !abfCompareTopDicts(a, b) &&
           std::equal(std::begin(a->FontBBox), std::end(a->FontBBox), std::begin(b->FontBBox)) &&
           same_string(a->Notice, b->Notice) && same_string(a->Copyright, b->Copyright) &&
           a->FSType == b->FSType &&
           a->FDArray.array[0].Private.initialRandomSeed ==
               b->FDArray.array[0].Private.initialRandomSeed;
}

bool FontProgram::has_shared_outlines(const FontProgram& other) const
{
    bool shared = false;
    for (const auto& [name, glyph] : glyphs()) {
        auto found = other.glyphs().find(name);
        if (found == other.glyphs().end())
            continue;
        if (glyph != found->second)
            return false;
        if (name != ".notdef" && !glyph.drawing.empty())
            shared = true;
    }
    return shared;
}

std::set<std::string> FontProgram::closure(std::set<std::string> keep) const
{
    keep.insert(".notdef");
    std::vector<std::string> pending(keep.begin(), keep.end());
    while (!pending.empty()) {
        auto name = std::move(pending.back());
        pending.pop_back();
        auto glyph = glyphs().find(name);
        if (glyph == glyphs().end())
            continue; // Missing names retain the original .notdef fallback.
        for (const auto& dependency : glyph->second.dependencies)
            if (keep.insert(dependency).second)
                pending.push_back(dependency);
    }
    return keep;
}

bool FontProgram::preserves(const FontProgram& source, const std::set<std::string>& keep) const
{
    if (!compatible(source))
        return false;
    for (const auto& name : keep) {
        auto before = source.glyphs().find(name);
        auto after = glyphs().find(name);
        // A missing component must stay missing. A union that supplies it can
        // change a seac glyph even when every PDF character mapping is intact.
        if (before == source.glyphs().end() && after != glyphs().end())
            return false;
        if (before != source.glyphs().end() &&
            (after == glyphs().end() || before->second != after->second))
            return false;
    }
    return true;
}

std::string FontProgram::cff(const std::vector<FontProgram*>& sources,
                             const std::set<std::string>* keep, const std::string& name)
{
    if (sources.empty())
        throw std::runtime_error("no font sources");
    IO io;
    std::unique_ptr<cfwCtx_, decltype(&cfwFree)> writer(
        cfwNew(&io.memory, &io.callbacks, CFW_CHECK_ARGS, font_logger()), cfwFree);
    if (!writer)
        throw std::runtime_error("cannot initialize CFF writer");
    check(cfwBegSet(writer.get(), CFW_SUBRIZE | CFW_NO_FAMILY_OPT | CFW_PRESERVE_FONT_BBOX),
          writer_message);
    check(cfwBegFont(writer.get(), nullptr, 0), writer_message);
    struct Selection {
        const std::set<std::string>* keep;
        std::set<std::string> emitted;
    } selection{keep, {}};
    auto callbacks = cfwGlyphCallbacks;
    callbacks.direct_ctx = writer.get();
    callbacks.indirect_ctx = &selection;
    callbacks.beg = [](abfGlyphCallbacks* cb, abfGlyphInfo* info) -> int {
        auto& selection = *static_cast<Selection*>(cb->indirect_ctx);
        std::string name = info->gname.ptr;
        if ((selection.keep && !selection.keep->contains(name)) ||
            !selection.emitted.insert(name).second)
            return ABF_SKIP_RET;
        info->flags &= ~ABF_GLYPH_SEEN;
        return cfwGlyphCallbacks.beg(cb, info);
    };
    for (auto* source : sources)
        source->impl_->iterate(callbacks);
    auto top = *sources.front()->impl_->top;
    auto font_dictionary = top.FDArray.array[0];
    if (!name.empty())
        font_dictionary.FontName.ptr = const_cast<char*>(name.c_str());
    top.FDArray.array = &font_dictionary;
    // Compatible sources have identical bounds. Keep the first dictionary intact.
    check(cfwEndFont(writer.get(), &top), writer_message);
    check(cfwEndSet(writer.get()), writer_message);
    return std::move(io.streams.at(CFW_DST_STREAM_ID).bytes);
}

} // namespace dpdfcomp
