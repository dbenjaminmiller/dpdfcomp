#include "image_forms.hpp"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace dpdfcomp
{
std::set<ObjectId> form_compatible_images(QPDF& pdf, bool include_forms)
{
    enum class Context { ordinary, resources, xobjects, image_reference };
    std::set<ObjectId> resource_dictionaries, resource_maps, unsafe, images;
    bool default_color_spaces = false;
    auto owns_resources = [](Object value) {
        auto type = value.getKey("/Type"), subtype = value.getKey("/Subtype");
        return type.isNameAndEquals("/Page") || type.isNameAndEquals("/Pages") ||
               type.isNameAndEquals("/Pattern") || subtype.isNameAndEquals("/Form") ||
               subtype.isNameAndEquals("/Type3");
    };
    auto objects = pdf.getAllObjects();
    // Discover resource containers without following arbitrary reference cycles.
    std::function<void(Object)> discover = [&](Object value) {
        if (value.isDictionary()) {
            for (const auto* name : {"/DefaultGray", "/DefaultRGB", "/DefaultCMYK"})
                default_color_spaces |= value.hasKey(name);
            if (owns_resources(value)) {
                auto resources = value.getKey("/Resources");
                if (resources.isDictionary()) {
                    if (resources.isIndirect())
                        resource_dictionaries.insert(resources.getObjGen());
                    auto map = resources.getKey("/XObject");
                    if (map.isDictionary() && map.isIndirect())
                        resource_maps.insert(map.getObjGen());
                }
            }
            for (const auto& key : value.getKeys()) {
                auto child = value.getKey(key);
                if (!child.isIndirect())
                    discover(child);
            }
        } else if (value.isArray()) {
            for (auto child : value.getArrayAsVector())
                if (!child.isIndirect())
                    discover(child);
        }
    };
    for (auto object : objects)
        discover(object.isStream() ? object.getDict() : object);
    // A new Form has its own resource scope. Preserve documents with default
    // color-space overrides until their inherited resource contexts are modeled.
    if (default_color_spaces)
        return {};
    auto mark_unsafe = [&](Object map) {
        if (map.isDictionary())
            for (const auto& key : map.getKeys()) {
                auto child = map.getKey(key);
                if (child.isIndirect())
                    unsafe.insert(child.getObjGen());
            }
    };
    std::function<void(Object, Context, bool)> visit = [&](Object value, Context context,
                                                           bool top) {
        if (value.isIndirect() && !top) {
            if (value.isStream() &&
                (value.getDict().getKey("/Subtype").isNameAndEquals("/Image") ||
                 (include_forms && value.getDict().getKey("/Subtype").isNameAndEquals("/Form"))))
                (context == Context::image_reference ? images : unsafe).insert(value.getObjGen());
            else if (resource_maps.contains(value.getObjGen()) && context != Context::xobjects)
                mark_unsafe(value);
            else if (resource_dictionaries.contains(value.getObjGen()) &&
                     context != Context::resources)
                mark_unsafe(value.getKey("/XObject"));
            return;
        }
        if (value.isStream())
            value = value.getDict();
        if (value.isDictionary()) {
            for (const auto& key : value.getKeys()) {
                auto next = Context::ordinary;
                if (context == Context::xobjects)
                    next = Context::image_reference;
                else if (context == Context::resources && key == "/XObject")
                    next = Context::xobjects;
                else if (key == "/Resources" && owns_resources(value))
                    next = Context::resources;
                visit(value.getKey(key), next, false);
            }
        } else if (value.isArray()) {
            for (auto child : value.getArrayAsVector())
                visit(child, Context::ordinary, false);
        }
    };
    visit(pdf.getTrailer(), Context::ordinary, true);
    for (auto object : objects) {
        auto context = resource_maps.contains(object.getObjGen())           ? Context::xobjects
                       : resource_dictionaries.contains(object.getObjGen()) ? Context::resources
                                                                            : Context::ordinary;
        visit(object, context, true);
    }

    const std::set<std::string> allowed{
        "/Type",   "/Subtype",     "/Width",  "/Height", "/BitsPerComponent", "/ColorSpace",
        "/Decode", "/Interpolate", "/Intent", "/Filter", "/DecodeParms",      "/Length"};
    const std::set<std::string> form_allowed{"/Type",      "/Subtype", "/FormType",    "/BBox",
                                             "/Resources", "/Filter",  "/DecodeParms", "/Length"};
    std::erase_if(images, [&](ObjectId id) {
        if (unsafe.contains(id))
            return true;
        auto dictionary = pdf.getObjectByObjGen(id).getDict();
        const auto& keys = include_forms && dictionary.getKey("/Subtype").isNameAndEquals("/Form")
                               ? form_allowed
                               : allowed;
        for (const auto& key : dictionary.getKeys())
            if (!keys.contains(key))
                return true;
        return false;
    });
    return images;
}

ImageForm make_image_form(int width, int height, std::vector<ImagePart> parts)
{
    std::ostringstream content;
    content.imbue(std::locale::classic());
    content << std::fixed << std::setprecision(15);
    std::string resources_text = "<< /XObject << ";
    std::size_t image_size = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        for (const auto& area : parts[i].placements) {
            if (area.x < 0 || area.y < 0 || area.width <= 0 || area.height <= 0 ||
                area.width > width - area.x || area.height > height - area.y)
                throw std::runtime_error("invalid image placement");
            content << "q " << double(area.width) / width << " 0 0 " << double(area.height) / height
                    << ' ' << double(area.x) / width << ' '
                    << double(height - area.y - area.height) / height << " cm /S" << i << " Do Q\n";
        }
        image_size += stored_size(parts[i].image);
        resources_text += "/S" + std::to_string(i) + " 2147483646 0 R ";
    }
    resources_text += ">> >>";
    auto dictionary =
        Object::parse("<< /Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 1 1] >>");
    auto bytes = content.str();
    dictionary.replaceKey("/Length", Object::newInteger(bytes.size()));
    Replacement form{dictionary, std::move(bytes), true};
    // Include new object/xref overhead and the longest possible resource refs.
    const auto size_bound =
        image_size + stored_size(form) + resources_text.size() + 16 + parts.size() * 80;
    return {std::move(form), std::move(parts), size_bound};
}

std::size_t commit_image_form(QPDF& pdf, Replacements& changes, Object source, ImageForm candidate)
{
    auto resources = Object::newDictionary();
    std::size_t image_size = 0;
    for (std::size_t i = 0; i < candidate.parts.size(); ++i) {
        auto& replacement = candidate.parts[i].image;
        image_size += stored_size(replacement);
        // QPDF allocates IDs; no existing object or resource dictionary is mutated.
        auto image = pdf.newStream(replacement.bytes);
        image.replaceDict(replacement.value);
        resources.replaceKey("/S" + std::to_string(i), image);
        changes.emplace(image.getObjGen(), std::move(replacement));
    }
    candidate.form.value.replaceKey("/Resources", Object::newDictionary({{"/XObject", resources}}));
    const auto original_size = stored_size(source);
    const auto replacement_size = image_size + stored_size(candidate.form);
    const auto savings = original_size > replacement_size ? original_size - replacement_size : 0;
    changes.emplace(source.getObjGen(), std::move(candidate.form));
    return savings;
}
} // namespace dpdfcomp
