#include "pdf.hpp"

#include <algorithm>
#include <qpdf/QPDFFormFieldObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{
void collect_signatures(Object value, std::vector<Object>& signatures, bool top = false)
{
    if (value.isIndirect() && !top)
        return; // Each indirect object is visited separately.
    if (value.isStream()) {
        value = value.getDict();
    } else if (value.isDictionary() && (value.getKey("/Type").isNameAndEquals("/Sig") ||
                                        value.getKey("/Type").isNameAndEquals("/DocTimeStamp"))) {
        signatures.push_back(value);
        return;
    }
    if (value.isDictionary()) {
        // /Type is optional on a signature value. /FT, including inheritance
        // from a parent field, identifies its role without guessing from keys
        // such as /ByteRange that also occur in unrelated private dictionaries.
        auto field_value = value.getKey("/V");
        if (field_value.isDictionary() && QPDFFormFieldObjectHelper(value).getFieldType() == "/Sig")
            signatures.push_back(field_value);
        for (const auto& key : value.getKeys())
            collect_signatures(value.getKey(key), signatures);
    } else if (value.isArray()) {
        for (auto item : value.getArrayAsVector())
            collect_signatures(item, signatures);
    }
}

Object unsigned_value(Object value, const std::vector<Object>& signatures, Object form,
                      bool& changed, bool top = false)
{
    if (std::any_of(signatures.begin(), signatures.end(),
                    [&](Object signature) { return value.isSameObjectAs(signature); })) {
        changed = true;
        return Object::newNull();
    }
    if (value.isIndirect() && !top)
        return value;
    if (!value.isDictionary() && !value.isArray())
        return value;
    auto copy = value.shallowCopy();
    if (value.isDictionary()) {
        for (const auto& key : value.getKeys()) {
            if (key == "/SigFlags" && value.isSameObjectAs(form)) {
                copy.removeKey(key);
                changed = true;
            } else {
                bool child_changed = false;
                auto child = unsigned_value(value.getKey(key), signatures, form, child_changed);
                if (child_changed) {
                    copy.replaceKey(key, child);
                    changed = true;
                }
            }
        }
    } else {
        for (int i = 0; i < value.getArrayNItems(); ++i) {
            bool child_changed = false;
            auto child = unsigned_value(value.getArrayItem(i), signatures, form, child_changed);
            if (child_changed) {
                copy.setArrayItem(i, child);
                changed = true;
            }
        }
    }
    return changed ? copy : value;
}

// Compare direct object values while translating indirect references through
// the linearizer's own object map. Stream lengths are checked through their bytes.
std::string canonical(Object value, QPDFWriter* writer, bool top = false)
{
    if (value.isNull())
        return "null";
    if (value.isIndirect() && !top) {
        auto id = writer ? writer->getRenumberedObjGen(value.getObjGen()) : value.getObjGen();
        return id.getObj() ? object_label(id) + " R" : "null";
    }
    if (value.isDictionary()) {
        std::string result = "<<";
        for (const auto& key : value.getKeys())
            result +=
                Object::newName(key).unparse() + " " + canonical(value.getKey(key), writer) + " ";
        return result + ">>";
    }
    if (value.isArray()) {
        std::string result = "[";
        for (auto item : value.getArrayAsVector())
            result += canonical(item, writer) + " ";
        return result + "]";
    }
    return value.unparseResolved();
}
} // namespace

bool remove_signatures(QPDF& pdf, Replacements& replacements)
{
    std::vector<Object> signatures;
    auto objects = pdf.getAllObjects();
    for (auto object : objects)
        collect_signatures(object, signatures, true);
    // Permission signatures may also omit /Type.
    auto permissions = pdf.getRoot().getKey("/Perms");
    if (permissions.isDictionary()) {
        for (const auto* key : {"/DocMDP", "/UR", "/UR3"}) {
            auto value = permissions.getKey(key);
            if (value.isDictionary())
                signatures.push_back(value);
        }
    }
    if (signatures.empty())
        return false;
    auto form = pdf.getRoot().getKey("/AcroForm");
    for (auto object : objects) {
        auto prior = replacements.find(object.getObjGen());
        auto value = prior != replacements.end() ? prior->second.value
                     : object.isStream()         ? object.getDict()
                                                 : object;
        bool changed = false;
        auto cleaned = unsigned_value(value, signatures, form, changed, true);
        if (changed) {
            const bool stream =
                prior != replacements.end() ? prior->second.stream : object.isStream();
            replacements[object.getObjGen()] = {
                cleaned,
                stream ? (prior != replacements.end() ? prior->second.bytes : raw_bytes(object))
                       : std::string{},
                stream};
        }
    }
    return true;
}

void linearize_pdf(const std::filesystem::path& input, const std::filesystem::path& output)
{
    QPDF pdf;
    pdf.setSuppressWarnings(true);
    pdf.setAttemptRecovery(false);
    pdf.processFile(utf8_path(input).c_str());
    const auto filename = utf8_path(output);
    QPDFWriter writer(pdf, filename.c_str());
    writer.setCompressStreams(false);
    writer.setDecodeLevel(qpdf_dl_none);
    writer.setDeterministicID(true);
    writer.forcePDFVersion(pdf.getPDFVersion());
    writer.setLinearization(true);
    writer.write();

    QPDF result;
    result.setSuppressWarnings(true);
    result.setAttemptRecovery(false);
    result.processFile(filename.c_str());
    if (!result.isLinearized() || !result.checkLinearization())
        throw std::runtime_error("verification failed: output is not correctly linearized");
    auto before_trailer = pdf.getTrailer().shallowCopy();
    auto after_trailer = result.getTrailer().shallowCopy();
    for (const auto* key : {"/Size", "/Prev", "/XRefStm", "/ID", "/Type", "/W", "/Index", "/Length",
                            "/Filter", "/DecodeParms"}) {
        before_trailer.removeKey(key);
        after_trailer.removeKey(key);
    }
    if (result.getPDFVersion() != pdf.getPDFVersion() ||
        canonical(before_trailer, &writer) != canonical(after_trailer, nullptr))
        throw std::runtime_error("verification failed after linearization: trailer or PDF version");
    for (auto source : pdf.getAllObjects()) {
        auto id = writer.getRenumberedObjGen(source.getObjGen());
        if (!id.getObj())
            continue; // Unreachable objects and superseded xref/hint storage.
        auto expected = source.isStream() ? source.getDict().shallowCopy() : source;
        if (source.isStream() && expected.getKey("/Type").isNameAndEquals("/ObjStm"))
            continue; // Storage for other objects; those objects are checked individually.
        auto actual_object = result.getObjectByObjGen(id);
        auto actual =
            actual_object.isStream() ? actual_object.getDict().shallowCopy() : actual_object;
        if (source.isStream())
            expected.removeKey("/Length");
        if (actual_object.isStream())
            actual.removeKey("/Length");
        if (source.isStream() != actual_object.isStream() ||
            canonical(expected, &writer, true) != canonical(actual, nullptr, true) ||
            (source.isStream() && raw_bytes(source) != raw_bytes(actual_object)))
            throw std::runtime_error("verification failed after linearization: object " +
                                     object_label(source.getObjGen()));
    }
    if (pdf.anyWarnings() || result.anyWarnings())
        throw std::runtime_error("linearization verification produced PDF warnings");
}
} // namespace dpdfcomp
