#include "reconstruction.hpp"
#include "compression.hpp"
#include "image_forms.hpp"
#include "image_pdf.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{
struct Tokens : Object::ParserCallbacks {
    std::vector<Object> values;
    void handleObject(Object value) override { values.push_back(value); }
    void handleEOF() override {}
};
struct Draw {
    Object image;
    std::string name;
    std::array<double, 6> matrix;
    int width, height;
    double row = 0;
};

bool near(double a, double b)
{
    return std::abs(a - b) <= 1e-7;
}

std::optional<ReconstructedImage> reconstruct(Object owner, Object content,
                                              const std::map<ObjectId, std::size_t>& references,
                                              const std::set<ObjectId>& eligible,
                                              const std::set<ObjectId>& consumed)
{
    const bool form = owner.isStream();
    auto dictionary = form ? owner.getDict() : owner;
    auto resources = dictionary.getKey("/Resources");
    auto unique = [&](Object object) {
        auto it = references.find(object.getObjGen());
        return !object.isIndirect() || (it != references.end() && it->second == 1);
    };
    if (!resources.isDictionary() || !unique(resources) || !content.isStream() ||
        (!form && !unique(content)))
        return std::nullopt;
    auto xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary() || !unique(xobjects))
        return std::nullopt;
    Tokens tokens;
    Object::parseContentStream(content, &tokens);
    // Optional enclosing q/Q has no state changes and can be removed safely.
    auto& values = tokens.values;
    auto op = [](Object value, const char* name) {
        return value.isOperator() && value.getOperatorValue() == name;
    };
    if (values.size() >= 2 && op(values.front(), "q") && op(values.back(), "Q") &&
        values.size() % 11 == 2) {
        values.erase(values.begin());
        values.pop_back();
    }
    if (values.size() < 22 || values.size() % 11)
        return std::nullopt;
    std::vector<Draw> draws;
    std::set<ObjectId> strips;
    Object attributes = Object::newNull();
    for (std::size_t start = 0; start < values.size(); start += 11) {
        if (!op(values[start], "q") || !op(values[start + 7], "cm") ||
            !values[start + 8].isName() || !op(values[start + 9], "Do") ||
            !op(values[start + 10], "Q"))
            return std::nullopt;
        Draw draw;
        for (int i = 0; i < 6; ++i) {
            if (!values[start + 1 + i].isNumber())
                return std::nullopt;
            draw.matrix[i] = values[start + 1 + i].getNumericValue();
            if (!std::isfinite(draw.matrix[i]))
                return std::nullopt;
        }
        draw.name = values[start + 8].getName();
        draw.image = xobjects.getKey(draw.name);
        if (!draw.image.isStream() ||
            !draw.image.getDict().getKey("/Subtype").isNameAndEquals("/Image") ||
            !eligible.contains(draw.image.getObjGen()) || !unique(draw.image) ||
            consumed.contains(draw.image.getObjGen()) ||
            !strips.insert(draw.image.getObjGen()).second)
            return std::nullopt;
        auto d = draw.image.getDict();
        if (!d.getKey("/Width").isInteger() || !d.getKey("/Height").isInteger() ||
            d.getKey("/Width").getIntValue() <= 0 ||
            d.getKey("/Width").getIntValue() > std::numeric_limits<int>::max() ||
            d.getKey("/Height").getIntValue() <= 0 ||
            d.getKey("/Height").getIntValue() > std::numeric_limits<int>::max())
            return std::nullopt;
        draw.width = static_cast<int>(d.getKey("/Width").getIntValue());
        draw.height = static_cast<int>(d.getKey("/Height").getIntValue());
        auto comparison = d.shallowCopy();
        for (auto key : {"/Height", "/Length", "/Filter", "/DecodeParms"})
            comparison.removeKey(key);
        if (attributes.isNull())
            attributes = comparison;
        else if (attributes.unparse() != comparison.unparse())
            return std::nullopt;
        draws.push_back(std::move(draw));
    }
    const auto first = draws.front();
    const auto& m = first.matrix;
    const double ux = m[0] / first.width, uy = m[1] / first.width;
    const double vx = m[2] / first.height, vy = m[3] / first.height;
    const auto determinant = ux * vy - uy * vx;
    if (!std::isfinite(determinant) || determinant == 0)
        return std::nullopt;
    std::size_t rows = 0;
    for (auto& draw : draws) {
        const auto& n = draw.matrix;
        // Compare in pixel coordinates, not page units: tolerance cannot hide
        // even a fraction of a sample on very high-resolution images.
        const auto local = [&](double x, double y) {
            return std::array<double, 2>{(x * vy - y * vx) / determinant,
                                         (y * ux - x * uy) / determinant};
        };
        auto x_axis = local(n[0], n[1]), y_axis = local(n[2], n[3]);
        auto origin = local(n[4] - m[4], n[5] - m[5]);
        if (!near(x_axis[0], draw.width) || !near(x_axis[1], 0) || !near(y_axis[0], 0) ||
            !near(y_axis[1], draw.height) || !near(origin[0], 0))
            return std::nullopt;
        draw.row = origin[1];
        rows += draw.height;
        if (rows > std::size_t(std::numeric_limits<int>::max()))
            return std::nullopt;
    }
    std::sort(draws.begin(), draws.end(),
              [](const Draw& a, const Draw& b) { return a.row > b.row; });
    for (std::size_t i = 1; i < draws.size(); ++i)
        if (!near(draws[i - 1].row, draws[i].row + draws[i].height))
            return std::nullopt;
    std::array<double, 6> matrix{
        m[0], m[1], vx * rows, vy * rows, draws.back().matrix[4], draws.back().matrix[5]};
    if (form) {
        auto box = dictionary.getKey("/BBox");
        if (!box.isArray() || box.getArrayNItems() != 4)
            return std::nullopt;
        for (int i = 0; i < 4; ++i)
            if (!box.getArrayItem(i).isNumber() ||
                !near(box.getArrayItem(i).getNumericValue(), i < 2 ? 0 : 1))
                return std::nullopt;
        const std::array<double, 6> identity{1, 0, 0, 1, 0, 0};
        for (int i = 0; i < 6; ++i)
            if (!std::isfinite(matrix[i]) || !near(matrix[i] * std::max(first.width, int(rows)),
                                                   identity[i] * std::max(first.width, int(rows))))
                return std::nullopt;
    }
    ReconstructedImage result;
    result.prototype = draws.front().image;
    result.target = form ? owner.getObjGen() : result.prototype.getObjGen();
    result.rows = static_cast<int>(rows);
    for (const auto& draw : draws) {
        result.strips.push_back(draw.image.getObjGen());
        result.original_size += stored_size(draw.image);
        if (draw.image.getObjGen() != result.target)
            result.related.emplace(draw.image.getObjGen(),
                                   Replacement{Object::newNull(), {}, false});
    }
    if (form)
        result.original_size += stored_size(owner);
    else {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << std::fixed << std::setprecision(17) << "q ";
        for (double value : matrix)
            text << value << ' ';
        text << "cm " << Object::newName(draws.front().name).unparse() << " Do Q\n";
        auto content_dictionary = content.getDict().shallowCopy();
        content_dictionary.removeKey("/Filter");
        content_dictionary.removeKey("/DecodeParms");
        content_dictionary.replaceKey("/Length", Object::newInteger(text.str().size()));
        Replacement replacement{content_dictionary, text.str(), true};
        result.related.emplace(content.getObjGen(), std::move(replacement));
        auto copy = dictionary.shallowCopy();
        auto new_resources = resources.shallowCopy(), new_xobjects = xobjects.shallowCopy();
        for (const auto& draw : draws)
            if (draw.image.getObjGen() != result.target)
                new_xobjects.removeKey(draw.name);
        new_resources.replaceKey("/XObject", new_xobjects);
        copy.replaceKey("/Resources", new_resources);
        result.related.emplace(owner.getObjGen(), Replacement{copy, {}, false});
        result.original_size +=
            stored_size(content) + stored_size(Replacement{dictionary, {}, false});
    }
    for (const auto& [id, replacement] : result.related)
        result.related_size += stored_size(replacement);
    return result;
}
} // namespace

Raster read_reconstructed_image(QPDF& pdf, const ReconstructedImage& image)
{
    Raster result{};
    for (auto id : image.strips) {
        auto strip = read_image(pdf.getObjectByObjGen(id));
        if (result.bytes.empty()) {
            result = Raster{strip.width, image.rows, strip.colors, strip.bits, {}, {}};
            if (result.row_bytes() > std::numeric_limits<std::size_t>::max() / image.rows)
                throw std::runtime_error("reconstructed image dimensions overflow");
            result.bytes.reserve(result.row_bytes() * image.rows);
        }
        if (strip.width != result.width || strip.colors != result.colors ||
            strip.bits != result.bits)
            throw std::runtime_error("decoded strips have incompatible sample layouts");
        result.bytes += strip.bytes;
    }
    if (result.bytes.size() != result.row_bytes() * result.height)
        throw std::runtime_error("reconstructed image length does not match its dimensions");
    return result;
}

std::vector<ReconstructedImage> reconstruct_images(QPDF& pdf)
{
    const auto references = reference_counts(pdf);
    const auto eligible = form_compatible_images(pdf, true);
    std::set<ObjectId> consumed;
    std::vector<ReconstructedImage> result;
    for (auto object : pdf.getAllObjects()) {
        Object content = Object::newNull();
        if (object.isStream() && object.getDict().getKey("/Subtype").isNameAndEquals("/Form") &&
            eligible.contains(object.getObjGen()))
            content = object;
        else if (object.isDictionary() && object.getKey("/Type").isNameAndEquals("/Page")) {
            content = object.getKey("/Contents");
            if (content.isArray() && content.getArrayNItems() == 1)
                content = content.getArrayItem(0);
        }
        if (!content.isStream())
            continue;
        try {
            if (auto image = reconstruct(object, content, references, eligible, consumed)) {
                consumed.insert(image->strips.begin(), image->strips.end());
                result.push_back(std::move(*image));
            }
        } catch (const std::exception&) {
            // An unsupported strip is preserved just like an unsupported image.
        }
    }
    return result;
}
} // namespace dpdfcomp
