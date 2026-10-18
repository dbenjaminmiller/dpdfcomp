#include "pdf.hpp"

#include "compression.hpp"
#include <qpdf/Buffer.hh>

#include <fstream>
#include <iomanip>
#include <set>
#include <stdexcept>
#include <vector>

namespace dpdfcomp
{
namespace
{

std::string buffer_string(const std::shared_ptr<Buffer>& buffer)
{
    return {reinterpret_cast<const char*>(buffer->getBuffer()), buffer->getSize()};
}

bool xref_storage(Object object, const std::map<ObjectId, std::size_t>& references)
{
    return object.isStream() && object.getDict().getKey("/Type").isNameAndEquals("/XRef") &&
           !references.contains(object.getObjGen());
}

void count_references(Object value, std::map<ObjectId, std::size_t>& counts, bool top = false)
{
    if (value.isIndirect() && !top) {
        ++counts[value.getObjGen()];
        return;
    }
    if (value.isStream())
        value = value.getDict();
    if (value.isDictionary()) {
        for (const auto& key : value.getKeys())
            count_references(value.getKey(key), counts);
    } else if (value.isArray()) {
        for (auto item : value.getArrayAsVector())
            count_references(item, counts);
    }
}

Object trailer_dictionary(QPDF& pdf)
{
    auto trailer = pdf.getTrailer().shallowCopy();
    bool xref = trailer.getKey("/Type").isNameAndEquals("/XRef");
    for (const auto* key : {"/Size", "/Prev", "/XRefStm"})
        trailer.removeKey(key);
    if (xref)
        for (const auto* key : {"/Type", "/W", "/Index", "/Length", "/Filter", "/DecodeParms"})
            trailer.removeKey(key);
    return trailer;
}

void write_object(std::ostream& out, ObjectId id, Object value, const std::string* bytes)
{
    out << id.getObj() << ' ' << id.getGen() << " obj\n" << value.unparseResolved();
    if (bytes) {
        out << "\nstream\n";
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        out << "\nendstream";
    }
    out << "\nendobj\n";
}

void append_big_endian(std::string& bytes, std::uint64_t value, int width)
{
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<char>((value >> shift) & 255));
}

std::string output_version(QPDF& pdf, const Replacements& replacements)
{
    PDFVersion required(1, 0);
    for (const auto& [id, replacement] : replacements) {
        if (!replacement.stream)
            continue;
        auto dictionary = replacement.value;
        auto filter = dictionary.getKey("/Filter");
        if (filter.isNameAndEquals("/JPXDecode"))
            required.updateIfGreater(PDFVersion(1, 5));
        if (filter.isNameAndEquals("/JBIG2Decode"))
            required.updateIfGreater(PDFVersion(1, 4));
        if (filter.isNameAndEquals("/FlateDecode") ||
            dictionary.getKey("/Subtype").isNameAndEquals("/Type1C"))
            required.updateIfGreater(PDFVersion(1, 2));
    }
    if (!(effective_version(pdf) < required))
        return pdf.getPDFVersion();
    std::string version;
    int extension;
    required.getVersion(version, extension);
    return version;
}

} // namespace

PDFVersion effective_version(QPDF& pdf)
{
    auto version = pdf.getVersionAsPDFVersion();
    auto catalog_version = pdf.getRoot().getKey("/Version");
    if (catalog_version.isName()) {
        const auto name = catalog_version.getName();
        if (name.size() == 4 && name[2] == '.' && name[1] >= '0' && name[1] <= '9' &&
            name[3] >= '0' && name[3] <= '9')
            version.updateIfGreater(PDFVersion(name[1] - '0', name[3] - '0'));
    }
    return version;
}

std::map<ObjectId, std::size_t> reference_counts(QPDF& pdf)
{
    std::map<ObjectId, std::size_t> counts;
    count_references(pdf.getTrailer(), counts);
    for (auto object : pdf.getAllObjects())
        count_references(object, counts, true);
    return counts;
}

std::string raw_bytes(Object stream)
{
    return buffer_string(stream.getRawStreamData());
}
std::string decoded_bytes(Object stream)
{
    return buffer_string(stream.getStreamData());
}

std::string object_label(ObjectId id)
{
    return std::to_string(id.getObj()) + " " + std::to_string(id.getGen());
}

std::size_t stored_size(Object stream)
{
    if (!stream.isStream())
        return stream.unparseResolved().size();
    return stream.getDict().unparseResolved().size() + raw_bytes(stream).size();
}

std::size_t stored_size(const Replacement& replacement)
{
    auto value = replacement.value;
    return value.unparseResolved().size() + replacement.bytes.size();
}

Replacement stream_replacement(Object stream, std::string bytes, const std::string& filter,
                               Object parameters)
{
    auto dictionary = stream.getDict().shallowCopy();
    dictionary.replaceKey("/Length", Object::newInteger(bytes.size()));
    dictionary.replaceKey("/Filter", Object::newName(filter));
    dictionary.replaceKey("/DecodeParms", parameters);
    return {dictionary, std::move(bytes), true};
}

std::vector<std::string> take_pdf_warnings(QPDF& pdf)
{
    std::map<std::string, std::size_t> counts;
    for (const auto& warning : pdf.getWarnings())
        ++counts[warning.getMessageDetail()];
    std::vector<std::string> warnings;
    for (const auto& [message, count] : counts) {
        auto text = "input PDF: " + message;
        if (count > 1)
            text += " (" + std::to_string(count) + " occurrences)";
        warnings.push_back(std::move(text));
    }
    return warnings;
}

void write_pdf(QPDF& pdf, const Replacements& replacements, const std::filesystem::path& path,
               bool preserve_object_streams)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "%PDF-" << output_version(pdf, replacements) << "\n%\342\343\317\323\n";
    auto original_xref = pdf.getXRefTable();
    auto references = reference_counts(pdf);
    struct Entry {
        int type;
        std::uint64_t first;
        int second;
    };
    std::map<int, Entry> entries;
    entries[0] = {0, 0, 65535};
    bool compressed_objects = false;
    int largest = 0;
    for (auto object : pdf.getAllObjects()) {
        auto id = object.getObjGen();
        largest = std::max(largest, id.getObj());
        if (xref_storage(object, references))
            continue;
        auto replacement = replacements.find(id);
        auto previous = original_xref.find(id);
        // Reusing an object-stream entry would also reuse its malformed syntax.
        // During repair, write the parsed object explicitly instead. The old
        // stream's bytes stay intact, but no xref entries point inside it.
        if (preserve_object_streams && replacement == replacements.end() &&
            previous != original_xref.end() &&
            previous->second.getType() == 2) {
            const auto& entry = previous->second;
            entries[id.getObj()] = {2, static_cast<std::uint64_t>(entry.getObjStreamNumber()),
                                    entry.getObjStreamIndex()};
            compressed_objects = true;
            continue;
        }
        entries[id.getObj()] = {1, static_cast<std::uint64_t>(out.tellp()), id.getGen()};
        if (replacement != replacements.end()) {
            const auto& item = replacement->second;
            write_object(out, id, item.value, item.stream ? &item.bytes : nullptr);
        } else if (object.isStream()) {
            auto bytes = raw_bytes(object);
            write_object(out, id, object.getDict(), &bytes);
        } else {
            write_object(out, id, object, nullptr);
        }
    }
    if (largest >= 2147483646)
        throw std::runtime_error("PDF object number limit reached");
    const auto xref_offset = static_cast<std::uint64_t>(out.tellp());
    auto trailer = trailer_dictionary(pdf);
    if (compressed_objects) {
        const int xref_id = largest + 1;
        entries[xref_id] = {1, xref_offset, 0};
        std::string bytes;
        auto index = Object::newArray();
        for (auto it = entries.begin(); it != entries.end();) {
            const int first = it->first;
            int count = 0;
            do {
                const auto& entry = it->second;
                append_big_endian(bytes, entry.type, 1);
                append_big_endian(bytes, entry.first, 8);
                append_big_endian(bytes, entry.second, 4);
                ++count;
                ++it;
            } while (it != entries.end() && it->first == first + count);
            index.appendItem(Object::newInteger(first));
            index.appendItem(Object::newInteger(count));
        }
        bytes = deflate(bytes);
        trailer.replaceKey("/Type", Object::newName("/XRef"));
        trailer.replaceKey("/Size", Object::newInteger(xref_id + 1));
        trailer.replaceKey("/W", Object::parse("[1 8 4]"));
        trailer.replaceKey("/Index", index);
        trailer.replaceKey("/Filter", Object::newName("/FlateDecode"));
        trailer.replaceKey("/Length", Object::newInteger(bytes.size()));
        write_object(out, ObjectId(xref_id, 0), trailer, &bytes);
    } else {
        out << "xref\n";
        for (auto it = entries.begin(); it != entries.end();) {
            auto first = it;
            int count = 0;
            do {
                ++count;
                ++it;
            } while (it != entries.end() && it->first == first->first + count);
            out << first->first << ' ' << count << '\n';
            for (auto entry = first; entry != it; ++entry) {
                if (entry->second.first > 9999999999ULL)
                    throw std::runtime_error("PDF exceeds classic xref offset limit");
                out << std::setfill('0') << std::setw(10) << entry->second.first << ' '
                    << std::setw(5) << entry->second.second
                    << (entry->second.type == 0 ? " f \n" : " n \n");
            }
        }
        trailer.replaceKey("/Size", Object::newInteger(entries.rbegin()->first + 1));
        out << "trailer\n" << trailer.unparseResolved() << '\n';
    }
    out << "startxref\n" << xref_offset << "\n%%EOF\n";
    out.close();
}

void verify_pdf(QPDF& original, const Replacements& replacements, const std::filesystem::path& path)
{
    auto references = reference_counts(original);
    QPDF output;
    output.setAttemptRecovery(false);
    output.processFile(utf8_path(path).c_str());
    if (output.getPDFVersion() != output_version(original, replacements))
        throw std::runtime_error("verification failed: PDF version changed unexpectedly");
    if (trailer_dictionary(original).unparseResolved() !=
        trailer_dictionary(output).unparseResolved())
        throw std::runtime_error("verification failed: trailer changed");
    for (auto source : original.getAllObjects()) {
        if (xref_storage(source, references))
            continue;
        auto id = source.getObjGen();
        auto result = output.getObjectByObjGen(id);
        auto replacement = replacements.find(id);
        const bool changed = replacement != replacements.end();
        bool stream = changed ? replacement->second.stream : source.isStream();
        auto expected = changed             ? replacement->second.value
                        : source.isStream() ? source.getDict()
                                            : source;
        auto actual = result.isStream() ? result.getDict() : result;
        if (result.isStream() != stream || actual.unparseResolved() != expected.unparseResolved())
            throw std::runtime_error("verification failed: object " + object_label(id));
        if (stream &&
            raw_bytes(result) != (changed ? replacement->second.bytes : raw_bytes(source)))
            throw std::runtime_error("verification failed: stream " + object_label(id));
    }
    if (output.anyWarnings())
        throw std::runtime_error("verification produced PDF warnings");
}

} // namespace dpdfcomp
