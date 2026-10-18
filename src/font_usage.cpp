#include "font_usage.hpp"

#include <functional>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{
struct Tokens : Object::ParserCallbacks {
    std::vector<Object> values;
    void handleObject(Object object) override { values.push_back(object); }
    void handleEOF() override {}
};
} // namespace

FontUsage::FontUsage(QPDF& pdf)
{
    const auto warnings_before = pdf.numWarnings();
    std::set<ObjectId> active, visited;
    std::function<void(Object, Object, Object, int)> scan;
    scan = [&](Object contents, Object resources, Object font, int depth) {
        if (depth > 64)
            throw std::runtime_error("recursive font resources");
        const auto id = contents.getObjGen();
        if (contents.isIndirect() && !active.insert(id).second)
            throw std::runtime_error("recursive font content");
        if (contents.isIndirect())
            visited.insert(id);
        Tokens tokens;
        Object::parseContentStream(contents, &tokens);
        std::vector<Object> arguments, stack;
        auto text = [&](Object value) {
            if (!value.isString() || !font.isIndirect())
                throw std::runtime_error("text without an identifiable font");
            strings_[font.getObjGen()].insert(value.getStringValue());
        };
        for (auto value : tokens.values) {
            if (!value.isOperator()) {
                arguments.push_back(value);
                continue;
            }
            const auto op = value.getOperatorValue();
            if (op == "q")
                stack.push_back(font);
            else if (op == "Q") {
                if (stack.empty())
                    throw std::runtime_error("unbalanced graphics state");
                font = stack.back();
                stack.pop_back();
            } else if (op == "Tf") {
                if (arguments.size() != 2 || !arguments[0].isName())
                    throw std::runtime_error("invalid font selection");
                font = resources.getKey("/Font").getKey(arguments[0].getName());
            } else if (op == "gs") {
                if (arguments.size() != 1 || !arguments[0].isName())
                    throw std::runtime_error("invalid graphics state selection");
                auto selection =
                    resources.getKey("/ExtGState").getKey(arguments[0].getName()).getKey("/Font");
                if (selection.isArray() && selection.getArrayNItems() == 2)
                    font = selection.getArrayItem(0);
            } else if (op == "Tj" || op == "'" || op == "\"") {
                if (arguments.empty())
                    throw std::runtime_error("missing text operand");
                text(arguments.back());
            } else if (op == "TJ") {
                if (arguments.size() != 1 || !arguments[0].isArray())
                    throw std::runtime_error("invalid text array");
                for (auto item : arguments[0].getArrayAsVector()) {
                    if (item.isString())
                        text(item);
                    else if (!item.isNumber())
                        throw std::runtime_error("invalid text array item");
                }
            } else if (op == "Do") {
                if (arguments.size() != 1 || !arguments[0].isName())
                    throw std::runtime_error("invalid XObject selection");
                auto form = resources.getKey("/XObject").getKey(arguments[0].getName());
                if (form.isStream() && form.getDict().getKey("/Subtype").isNameAndEquals("/Form")) {
                    auto local = form.getDict().getKey("/Resources");
                    scan(form, local.isDictionary() ? local : resources, font, depth + 1);
                }
            }
            arguments.clear();
        }
        active.erase(id);
    };
    try {
        for (auto page : QPDFPageDocumentHelper(pdf).getAllPages()) {
            auto contents = page.getObjectHandle().getKey("/Contents");
            if (!contents.isNull())
                scan(contents, page.getAttribute("/Resources", false), Object::newNull(), 0);
        }
        // Include annotation appearances, tiling patterns, and soft-mask forms,
        // including objects not reached through a page's Do operators.
        for (auto object : pdf.getAllObjects()) {
            if (object.isDictionary() && object.getKey("/Subtype").isNameAndEquals("/Type3"))
                complete_ = false; // CharProcs can inherit arbitrary text state.
            if (!object.isStream() || visited.contains(object.getObjGen()))
                continue;
            auto dictionary = object.getDict();
            if (dictionary.getKey("/Subtype").isNameAndEquals("/Form") ||
                dictionary.hasKey("/PatternType"))
                scan(object, dictionary.getKey("/Resources"), Object::newNull(), 0);
        }
        // Interactive fields may display values absent from their cached appearance.
        auto form = pdf.getRoot().getKey("/AcroForm");
        if (form.isDictionary())
            complete_ = false;
    } catch (const std::exception&) {
        complete_ = false;
    }
    if (pdf.numWarnings() != warnings_before)
        complete_ = false;
}

std::optional<std::set<unsigned int>> FontUsage::codes(Object font, int bytes_per_code) const
{
    auto found = strings_.find(font.getObjGen());
    if (!complete_ || found == strings_.end())
        return std::nullopt;
    std::set<unsigned int> codes;
    for (const auto& text : found->second) {
        if (text.size() % bytes_per_code)
            return std::nullopt;
        for (std::size_t i = 0; i < text.size(); i += bytes_per_code) {
            unsigned int code = 0;
            for (int j = 0; j < bytes_per_code; ++j)
                code = (code << 8) | static_cast<unsigned char>(text[i + j]);
            codes.insert(code);
        }
    }
    return codes;
}
} // namespace dpdfcomp
