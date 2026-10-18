#pragma once

#include <qpdf/QPDF.hh>

#include <filesystem>
#include <map>
#include <string>

namespace dpdfcomp
{

using Object = QPDFObjectHandle;
using ObjectId = QPDFObjGen;

// QPDF uses UTF-8 filenames on every platform, including Windows.
inline std::string utf8_path(const std::filesystem::path& path)
{
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

// A replacement owns its bytes and a direct dictionary. Parallel codec tasks
// never access QPDF; extraction and planning serialize access to the document.
struct Replacement {
    Object value;
    std::string bytes;
    bool stream = false;
};
using Replacements = std::map<ObjectId, Replacement>;

std::map<ObjectId, std::size_t> reference_counts(QPDF& pdf);
PDFVersion effective_version(QPDF& pdf);

std::string raw_bytes(Object stream);
std::string decoded_bytes(Object stream);
std::string object_label(ObjectId id);
std::size_t stored_size(Object stream);
std::size_t stored_size(const Replacement& replacement);
Replacement stream_replacement(Object stream, std::string bytes, const std::string& filter,
                               Object parameters = Object::newNull());

// Preserve object numbers, generation numbers, original object streams, trailer
// metadata, and every unrelated encoded stream. Only xref storage is rebuilt.
void write_pdf(QPDF& pdf, const Replacements& replacements, const std::filesystem::path& path);
void verify_pdf(QPDF& original, const Replacements& replacements,
                const std::filesystem::path& path);
std::vector<std::string> check_document(QPDF& pdf);
// Signature data is removed through the same replacement plan as compression.
// Returns true when the input contained a signature.
bool remove_signatures(QPDF& pdf, Replacements& replacements);
// Rebuild linearization and verify objects after QPDF's required renumbering.
void linearize_pdf(const std::filesystem::path& input, const std::filesystem::path& output);

} // namespace dpdfcomp
