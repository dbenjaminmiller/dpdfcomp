#include "images.hpp"

#include "bilevel.hpp"
#include "compression.hpp"
#include "image_codecs.hpp"
#include "image_forms.hpp"
#include "image_pdf.hpp"
#include "reconstruction.hpp"
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace dpdfcomp
{
namespace
{

struct Variant {
    Raster raster{};
    std::vector<Encoding> candidates;
    struct Stripe {
        int rows;
        std::vector<Encoding> candidates;
    };
    struct Plan {
        ImageBounds area;
        std::vector<Stripe> stripes;
        std::optional<std::string> white;
        std::vector<Encoding> white_candidates;
    };
    std::vector<Plan> plans;
    bool stripe = false;
    bool can_form = false;
    std::optional<std::string> white;
    bool converted = false;
    bool threshold = false;
    bool device_color = false;
    std::vector<ColorEncoding> color_codecs;
    std::optional<std::array<int, 2>> color_key;
    std::optional<std::string> mask;
    bool mask_flate = false;
};
struct Work {
    Object source; // Only accessed in the serial pipeline stages.
    ObjectId target;
    std::optional<ReconstructedImage> reconstruction;
    std::vector<Variant> variants;
    BilevelInput bilevel;
    bool originally_bilevel = false;
    bool original_allowed = true;
    bool specified_only = false;
    bool detected = false;
    bool transparency_mask = false;
    bool thresholds_skipped = false;
    std::string error;
};

std::vector<Encoding> compress_raster(const Raster& raster, const Options& options,
                                      const Variant& variant)
{
    return raster.bilevel() ? encode_bilevel(raster, options)
                            : encode_color(raster, variant.color_codecs, variant.device_color);
}

void compress(Variant& work, const Options& options)
{
    auto& raster = work.raster;
    if (options.crop_internal && options.jbig2 && raster.bilevel())
        raster.bounds = foreground(raster);

    // Find white bounds before any crop, stripe, or codec operation.
    std::optional<ImageBounds> external_bounds;
    if (options.crop_internal && work.can_form && work.white &&
        (!raster.bilevel() || options.bilevel_flate || options.bilevel_png_iterations ||
         options.ccitt)) {
        auto area = white_bounds(raster, *work.white);
        if (area.width && area.height) {
            // Retain a white neighbor at each cut so interpolation at the new
            // image boundary does not extend a foreground pixel into the margin.
            const auto right =
                std::min(raster.width, area.x + area.width + (area.x + area.width < raster.width));
            const auto bottom = std::min(raster.height, area.y + area.height +
                                                            (area.y + area.height < raster.height));
            area.x = std::max(0, area.x - 1);
            area.y = std::max(0, area.y - 1);
            area.width = right - area.x;
            area.height = bottom - area.y;
        }
        if (area.width != raster.width || area.height != raster.height)
            external_bounds = area;
    }
    if (!work.stripe)
        work.candidates = compress_raster(raster, options, work);

    auto add_plan = [&](const Raster& image, ImageBounds area, const Options& codecs,
                        std::optional<std::string> white) {
        Variant::Plan plan{area, {}, std::move(white), {}};
        if (plan.white) {
            Raster pixel{1, 1, raster.colors, raster.bits, *plan.white, {}};
            plan.white_candidates = compress_raster(pixel, codecs, work);
            // An empty crop still needs a compatible encoder. Otherwise its
            // synthetic white image could bypass disabled or filtered codecs.
            if (plan.white_candidates.empty())
                return;
        }
        const auto rows_per_strip =
            options.pdf_stripe_rows ? options.pdf_stripe_rows : image.height;
        const auto count = image.height ? 1 + (image.height - 1) / rows_per_strip : 0;
        plan.stripes.resize(count);
        oneapi::tbb::parallel_for(0, count, [&](int i) {
            const auto y = i * rows_per_strip;
            const auto rows = std::min(rows_per_strip, image.height - y);
            Raster tile{image.width,
                        rows,
                        image.colors,
                        image.bits,
                        image.bytes.substr(std::size_t(y) * image.row_bytes(),
                                           std::size_t(rows) * image.row_bytes()),
                        {}};
            if (image.bounds) {
                auto bounds = *image.bounds;
                const auto top = std::max(y, bounds.y);
                const auto bottom = std::min(y + rows, bounds.y + bounds.height);
                tile.bounds = bottom > top
                                  ? ImageBounds{bounds.x, top - y, bounds.width, bottom - top}
                                  : ImageBounds{};
            }
            plan.stripes[i] = {rows, compress_raster(tile, codecs, work)};
        });
        work.plans.push_back(std::move(plan));
    };
    if (work.stripe)
        add_plan(raster, {0, 0, raster.width, raster.height}, options, std::nullopt);
    if (external_bounds) {
        auto codecs = options;
        codecs.jbig2 = false; // JBIG2 already has the cheaper full-canvas internal crop candidate.
        add_plan(crop(raster, *external_bounds), *external_bounds, codecs, work.white);
    }
}

Replacement replacement(Object source, const Raster& raster, Encoding encoding,
                        const Variant& variant)
{
    auto parameters = Object::newDictionary();
    std::string filter;
    switch (encoding.codec) {
    case Codec::flate:
        filter = "/FlateDecode";
        parameters = Object::newNull();
        break;
    case Codec::lzw:
    case Codec::zip:
    case Codec::png:
        filter = encoding.codec == Codec::lzw ? "/LZWDecode" : "/FlateDecode";
        parameters.replaceKey("/Predictor", Object::newInteger(encoding.predictor));
        parameters.replaceKey("/Columns", Object::newInteger(raster.width));
        parameters.replaceKey("/Colors", Object::newInteger(raster.colors));
        parameters.replaceKey("/BitsPerComponent", Object::newInteger(raster.bits));
        break;
    case Codec::jpeg:
        filter = "/DCTDecode";
        parameters.replaceKey("/ColorTransform", Object::newInteger(raster.colors == 3 ? 1 : 0));
        break;
    case Codec::jp2:
        filter = "/JPXDecode";
        parameters = Object::newNull();
        break;
    case Codec::ccitt:
        filter = "/CCITTFaxDecode";
        parameters.replaceKey("/K", Object::newInteger(-1));
        parameters.replaceKey("/Columns", Object::newInteger(raster.width));
        parameters.replaceKey("/Rows", Object::newInteger(raster.height));
        parameters.replaceKey("/BlackIs1", Object::newBool(true));
        break;
    case Codec::jbig2:
        filter = "/JBIG2Decode";
        parameters = Object::newNull();
        break;
    }
    auto result = stream_replacement(source, std::move(encoding.bytes), filter, parameters);
    if (source.getDict().getKey("/Width").getIntValue() != raster.width)
        result.value.replaceKey("/Width", Object::newInteger(raster.width));
    if (source.getDict().getKey("/Height").getIntValue() != raster.height)
        result.value.replaceKey("/Height", Object::newInteger(raster.height));
    if (!result.value.hasKey("/BitsPerComponent") || encoding.bits)
        result.value.replaceKey("/BitsPerComponent",
                                Object::newInteger(encoding.bits ? encoding.bits : raster.bits));
    if (encoding.bits && encoding.bits != raster.bits) {
        auto mask = result.value.getKey("/Mask");
        if (mask.isArray()) {
            auto scaled = Object::newArray();
            const auto factor = ((1 << encoding.bits) - 1) / ((1 << raster.bits) - 1);
            for (auto value : mask.getArrayAsVector()) {
                if (!value.isInteger())
                    throw std::runtime_error("invalid color key mask");
                scaled.appendItem(Object::newInteger(value.getIntValue() * factor));
            }
            result.value.replaceKey("/Mask", scaled);
        }
    }
    result.value.removeKey("/SMaskInData");
    if (variant.converted) {
        result.value.replaceKey("/BitsPerComponent", Object::newInteger(1));
        auto original_space = source.getDict().getKey("/ColorSpace");
        result.value.replaceKey(
            "/ColorSpace",
            original_space.isNameAndEquals("/DeviceRGB")
                ? Object::newArray({Object::newName("/Indexed"), original_space,
                                    Object::newInteger(1),
                                    Object::newString(std::string("\0\0\0\xff\xff\xff", 6))})
                : Object::newName("/DeviceGray"));
        result.value.removeKey("/Decode");
        if (source.getDict().getKey("/Mask").isArray()) {
            result.value.removeKey("/Mask");
            if (variant.color_key)
                result.value.replaceKey(
                    "/Mask", Object::newArray({Object::newInteger((*variant.color_key)[0]),
                                               Object::newInteger((*variant.color_key)[1])}));
            else if (variant.mask)
                result.value.replaceKey("/Mask", Object::newNull());
        }
    }
    return result;
}

std::optional<ImageForm> prepare_form(Object source, const Raster& raster, Variant::Plan& plan,
                                      const Variant& variant, bool specified_only, bool& lossy)
{
    std::vector<ImagePart> parts;
    int y = plan.area.y;
    bool complete = true;
    for (auto& stripe : plan.stripes) {
        std::optional<Replacement> image;
        Raster shape{plan.area.width, stripe.rows, raster.colors, raster.bits, {}, {}};
        bool stripe_lossy = false;
        for (auto& encoding : stripe.candidates) {
            const auto candidate_lossy = encoding.lossy;
            auto candidate = replacement(source, shape, std::move(encoding), variant);
            if (!image || stored_size(candidate) < stored_size(*image)) {
                image = std::move(candidate);
                stripe_lossy = candidate_lossy;
            }
        }
        if (!image) {
            complete = false;
            break;
        }
        lossy |= stripe_lossy;
        parts.push_back({std::move(*image), {{plan.area.x, y, plan.area.width, stripe.rows}}});
        y += stripe.rows;
    }
    if (!complete)
        return std::nullopt;
    if (plan.white) {
        Raster pixel{1, 1, raster.colors, raster.bits, {}, {}};
        std::optional<Replacement> image;
        bool background_lossy = false;
        if (!specified_only) {
            image = replacement(source, pixel, Encoding{Codec::flate, *plan.white}, variant);
            image->value.removeKey("/Filter");
            image->value.removeKey("/DecodeParms");
        }
        for (auto& encoding : plan.white_candidates) {
            const auto candidate_lossy = encoding.lossy;
            auto candidate = replacement(source, pixel, std::move(encoding), variant);
            if (!image || stored_size(candidate) < stored_size(*image)) {
                image = std::move(candidate);
                background_lossy = candidate_lossy;
            }
        }
        if (!image)
            return std::nullopt;
        lossy |= background_lossy;
        ImagePart background{std::move(*image), {}};
        const auto width = raster.width, height = raster.height;
        const auto area = plan.area;
        // Paint disjoint margins, never a background beneath foreground
        // pixels: overlapping painting could change alpha/blend behavior.
        for (auto edge : std::vector<ImageBounds>{
                 {0, 0, width, area.y},
                 {0, area.y + area.height, width, height - area.y - area.height},
                 {0, area.y, area.x, area.height},
                 {area.x + area.width, area.y, width - area.x - area.width, area.height}})
            if (edge.width && edge.height)
                background.placements.push_back(edge);
        parts.push_back(std::move(background));
    }
    return make_image_form(raster.width, raster.height, std::move(parts));
}

} // namespace

void optimize_images(QPDF& pdf, Replacements& changes, const Options& options, Report& report)
{
    auto codec_options = options;
    const auto version = effective_version(pdf);
    if (!options.upgrade_version) {
        std::set<std::string> warned;
        for (const auto& codec : options.explicit_codecs) {
            const auto required = codec.starts_with("jp2-") ? PDFVersion(1, 5)
                                  : codec == "jbig2"        ? PDFVersion(1, 4)
                                  : (codec == "flate" || codec == "zip" || codec == "png" ||
                                     codec.starts_with("png-"))
                                      ? PDFVersion(1, 2)
                                      : PDFVersion(1, 0);
            if (version < required && warned.insert(codec).second) {
                auto warning =
                    "codec " + codec +
                    " not used because it is not supported in specified PDF output version " +
                    std::to_string(version.getMajor()) + "." + std::to_string(version.getMinor());
                report.warnings.push_back(warning);
                if (options.warning)
                    options.warning(warning);
            }
        }
    }
    const bool flate_allowed = options.upgrade_version || !(version < PDFVersion(1, 2));
    codec_options.bilevel_flate &= flate_allowed;
    if (!flate_allowed)
        codec_options.bilevel_png_iterations = 0;
    codec_options.jbig2 =
        options.jbig2 && (options.upgrade_version || !(version < PDFVersion(1, 4)));
    auto allowed = [&](ColorEncoding encoding) {
        if ((encoding.codec == ColorCodec::png || encoding.codec == ColorCodec::zip) &&
            !flate_allowed)
            return false;
        return options.upgrade_version || !(version < PDFVersion(1, 5)) ||
               (encoding.codec != ColorCodec::jp2_lossless &&
                encoding.codec != ColorCodec::jp2_lossy);
    };
    std::erase_if(codec_options.color_lossless, [&](auto c) { return !allowed(c); });
    std::erase_if(codec_options.color_lossy, [&](auto c) { return !allowed(c); });
    auto reconstructed =
        options.reconstruct_strips ? reconstruct_images(pdf) : std::vector<ReconstructedImage>{};
    std::map<ObjectId, std::size_t> reconstructed_index;
    std::set<ObjectId> consumed;
    for (std::size_t i = 0; i < reconstructed.size(); ++i) {
        auto& image = reconstructed[i];
        reconstructed_index.emplace(image.prototype.getObjGen(), i);
        consumed.insert(image.strips.begin(), image.strips.end());
    }
    auto objects = pdf.getAllObjects();
    const auto masks = transparency_masks(pdf);
    const auto compatible = (options.pdf_stripe_rows || options.crop_internal)
                                ? form_compatible_images(pdf)
                                : std::set<ObjectId>{};
    std::erase_if(objects, [&](Object object) {
        return !object.isStream() ||
               !object.getDict().getKey("/Subtype").isNameAndEquals("/Image") ||
               (consumed.contains(object.getObjGen()) &&
                !reconstructed_index.contains(object.getObjGen()));
    });
    Progress progress{Progress::Kind::images};
    progress.total = objects.size();
    if (options.progress)
        options.progress(progress);
    auto next = objects.begin();
    // Serial stages can overlap. All QPDF access shares this mutex; pixel and
    // codec work operates only on owned values, scheduled by oneTBB.
    std::mutex pdf_mutex;
    auto threads = options.threads ? options.threads : oneapi::tbb::info::default_concurrency();
    oneapi::tbb::parallel_pipeline(
        std::max(1, threads * 2),
        oneapi::tbb::make_filter<void, std::shared_ptr<Work>>(
            oneapi::tbb::filter_mode::serial_in_order,
            [&](oneapi::tbb::flow_control& flow) -> std::shared_ptr<Work> {
                if (next == objects.end()) {
                    flow.stop();
                    return {};
                }
                auto work = std::make_shared<Work>();
                std::lock_guard lock(pdf_mutex);
                work->source = *next++;
                work->target = work->source.getObjGen();
                work->transparency_mask = masks.contains(work->target);
                if (auto it = reconstructed_index.find(work->target);
                    it != reconstructed_index.end()) {
                    work->reconstruction = std::move(reconstructed[it->second]);
                    work->target = work->reconstruction->target;
                }
                try {
                    auto d = work->source.getDict();
                    auto space = d.getKey("/ColorSpace");
                    const bool mask =
                        d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue();
                    work->originally_bilevel =
                        mask || (d.getKey("/BitsPerComponent").isInteger() &&
                                 d.getKey("/BitsPerComponent").getIntValue() == 1 &&
                                 components(space) == 1);
                    bool originally_lossy = lossy_input(d.getKey("/Filter"));
                    if (work->reconstruction)
                        for (auto id : work->reconstruction->strips)
                            originally_lossy |=
                                lossy_input(pdf.getObjectByObjGen(id).getDict().getKey("/Filter"));
                    auto selected_color =
                        originally_lossy ? options.color_lossy : options.color_lossless;
                    // Thresholding would harden soft transparency edges. Exact black and
                    // white detection still applies, since it is lossless.
                    auto threshold = [](ColorEncoding c) { return c.codec == ColorCodec::bilevel; };
                    if (work->transparency_mask)
                        work->thresholds_skipped = std::erase_if(selected_color, threshold) > 0;
                    const bool bilevel_enabled = options.recompress_bilevel &&
                                                 (options.bilevel_flate || options.ccitt ||
                                                  options.jbig2 || options.bilevel_png_iterations);
                    auto classify = [&] {
                        work->specified_only =
                            work->originally_bilevel
                                ? (bilevel_enabled && options.bilevel_specified_only)
                                : (!selected_color.empty() &&
                                   (originally_lossy ? options.color_lossy_specified_only
                                                     : options.color_lossless_specified_only));
                        work->original_allowed =
                            !work->specified_only ||
                            matches_codec(work->source, options, work->originally_bilevel,
                                          selected_color);
                    };
                    classify();
                    if (work->originally_bilevel && !options.recompress_bilevel)
                        throw std::runtime_error("existing bilevel image compression is disabled");
                    Variant variant;
                    variant.color_codecs =
                        originally_lossy ? codec_options.color_lossy : codec_options.color_lossless;
                    if (work->transparency_mask)
                        std::erase_if(variant.color_codecs, threshold);
                    if (!work->originally_bilevel && variant.color_codecs.empty() &&
                        !options.detect_bilevel && d.hasKey("/BitsPerComponent"))
                        throw std::runtime_error("color image compression is disabled");
                    variant.raster = work->reconstruction
                                         ? read_reconstructed_image(pdf, *work->reconstruction)
                                         : read_image(work->source);
                    // JPX may omit BitsPerComponent; its codestream supplies it.
                    if (!work->originally_bilevel && variant.raster.bilevel()) {
                        work->originally_bilevel = true;
                        classify();
                        if (!options.recompress_bilevel)
                            throw std::runtime_error(
                                "existing bilevel image compression is disabled");
                    }
                    if (work->reconstruction && work->specified_only)
                        for (auto id : work->reconstruction->strips)
                            work->original_allowed &=
                                matches_codec(pdf.getObjectByObjGen(id), options,
                                              work->originally_bilevel, selected_color);
                    variant.device_color =
                        space.isNameAndEquals("/DeviceGray") || space.isNameAndEquals("/DeviceRGB");
                    // JPX ignores Decode arrays, unlike the other codecs. Do not
                    // silently drop that mapping or reinterpret color-key masks.
                    if (!d.getKey("/Decode").isNull())
                        std::erase_if(variant.color_codecs, [](auto c) {
                            return c.codec == ColorCodec::jp2_lossless ||
                                   c.codec == ColorCodec::jp2_lossy;
                        });
                    if (d.getKey("/Mask").isArray())
                        std::erase_if(variant.color_codecs, [](auto c) {
                            return c.codec == ColorCodec::jpeg || c.codec == ColorCodec::jp2_lossy;
                        });
                    work->bilevel = bilevel_input(work->source, variant.raster);
                    variant.can_form = compatible.contains(work->source.getObjGen());
                    if (options.crop_internal && variant.can_form)
                        variant.white = white_pixel(work->source, variant.raster);
                    variant.stripe = options.pdf_stripe_rows && variant.can_form &&
                                     variant.raster.height > options.pdf_stripe_rows;
                    work->variants.push_back(std::move(variant));
                } catch (const std::exception& e) {
                    work->error = e.what();
                }
                return work;
            }) &
            oneapi::tbb::make_filter<std::shared_ptr<Work>, std::shared_ptr<Work>>(
                oneapi::tbb::filter_mode::parallel,
                [&](std::shared_ptr<Work> work) {
                    if (!work->error.empty())
                        return work;
                    try {
                        auto& original = work->variants.front();
                        std::vector<ColorEncoding> conversions;
                        for (auto encoding : original.color_codecs)
                            if (encoding.codec == ColorCodec::bilevel) {
                                if (encoding.smooth)
                                    encoding.parameter = 0; // Otsu's method chooses instead.
                                if (std::none_of(conversions.begin(), conversions.end(),
                                                 [&](auto other) {
                                                     return other.parameter == encoding.parameter &&
                                                            other.scale == encoding.scale &&
                                                            other.smooth == encoding.smooth;
                                                 }))
                                    conversions.push_back(encoding);
                            }
                        auto converted =
                            !original.raster.bilevel() && options.detect_bilevel
                                ? convert_bilevel(original.raster, work->bilevel, std::nullopt)
                                : std::nullopt;
                        work->detected = converted.has_value();
                        auto make_variant = [&](BilevelResult result, bool threshold) {
                            Variant variant;
                            variant.raster = std::move(result.raster);
                            variant.converted = true;
                            variant.threshold = threshold;
                            variant.color_key = result.color_key;
                            variant.mask = std::move(result.mask);
                            variant.can_form = work->variants.front().can_form && !variant.mask &&
                                               !variant.color_key;
                            variant.stripe = work->variants.front().stripe && variant.can_form;
                            if (variant.can_form)
                                variant.white = std::string(1, char(0x80));
                            return variant;
                        };
                        if (converted) {
                            auto variant = make_variant(std::move(*converted), false);
                            work->variants.clear();
                            work->variants.push_back(std::move(variant));
                            work->specified_only =
                                options.bilevel_specified_only &&
                                (!options.recompress_bilevel || options.bilevel_flate ||
                                 options.ccitt || options.jbig2 || options.bilevel_png_iterations);
                        } else {
                            for (auto conversion : conversions)
                                if (!work->variants.front().raster.bilevel())
                                    if (auto result = convert_bilevel(work->variants.front().raster,
                                                                      work->bilevel, conversion))
                                        work->variants.push_back(
                                            make_variant(std::move(*result), true));
                        }
                        for (auto& variant : work->variants) {
                            auto codecs = codec_options;
                            if (variant.converted && !options.recompress_bilevel) {
                                codecs.bilevel_flate = flate_allowed;
                                codecs.ccitt = true;
                                codecs.jbig2 = false;
                                codecs.bilevel_png_iterations = 0;
                            }
                            if (variant.mask && flate_allowed) {
                                auto compressed = deflate(*variant.mask);
                                if (compressed.size() < variant.mask->size()) {
                                    variant.mask = std::move(compressed);
                                    variant.mask_flate = true;
                                }
                            }
                            compress(variant, codecs);
                        }
                    } catch (const std::exception& e) {
                        work->error = e.what();
                    }
                    return work;
                }) &
            oneapi::tbb::make_filter<std::shared_ptr<Work>, void>(
                oneapi::tbb::filter_mode::serial_in_order, [&](std::shared_ptr<Work> work) {
                    {
                        std::lock_guard lock(pdf_mutex);
                        const auto original_size = work->reconstruction
                                                       ? work->reconstruction->original_size
                                                       : stored_size(work->source);
                        const auto related_size =
                            work->reconstruction ? work->reconstruction->related_size : 0;
                        if (work->detected) {
                            auto converted_options = options;
                            if (!options.recompress_bilevel) {
                                converted_options.bilevel_flate = converted_options.ccitt = true;
                                converted_options.jbig2 = false;
                                converted_options.bilevel_png_iterations = 0;
                            }
                            work->original_allowed =
                                !work->specified_only ||
                                matches_codec(work->source, converted_options, true, {});
                            if (work->reconstruction && work->specified_only)
                                for (auto id : work->reconstruction->strips)
                                    work->original_allowed &= matches_codec(
                                        pdf.getObjectByObjGen(id), converted_options, true, {});
                        }
                        auto size = work->original_allowed
                                        ? original_size
                                        : std::numeric_limits<std::size_t>::max();
                        std::optional<Replacement> best;
                        std::optional<Replacement> best_mask;
                        std::optional<ImageForm> form;
                        std::size_t chosen_strips = 0;
                        bool cropped = false, converted = false, lossy = false;
                        for (auto& variant : work->variants) {
                            std::optional<Replacement> mask;
                            if (variant.mask) {
                                auto d = Object::parse("<< /Type /XObject /Subtype /Image "
                                                       "/ImageMask true /BitsPerComponent 1 >>");
                                d.replaceKey("/Width", Object::newInteger(variant.raster.width));
                                d.replaceKey("/Height", Object::newInteger(variant.raster.height));
                                d.replaceKey("/Length", Object::newInteger(variant.mask->size()));
                                if (variant.mask_flate)
                                    d.replaceKey("/Filter", Object::newName("/FlateDecode"));
                                mask = Replacement{d, *variant.mask, true};
                            }
                            const auto mask_cost = mask ? stored_size(*mask) + 100 : 0;
                            for (auto& encoding : variant.candidates) {
                                const bool candidate_lossy = variant.threshold || encoding.lossy;
                                auto candidate = replacement(work->source, variant.raster,
                                                             std::move(encoding), variant);
                                if (stored_size(candidate) + mask_cost + related_size < size) {
                                    size = stored_size(candidate) + mask_cost + related_size;
                                    best = std::move(candidate);
                                    best_mask = mask;
                                    form.reset();
                                    converted = variant.converted;
                                    lossy = candidate_lossy;
                                }
                            }
                            for (auto& plan : variant.plans) {
                                bool candidate_lossy = variant.threshold;
                                auto candidate =
                                    prepare_form(work->source, variant.raster, plan, variant,
                                                 work->specified_only, candidate_lossy);
                                if (candidate && candidate->size_bound + related_size < size) {
                                    size = candidate->size_bound + related_size;
                                    form = std::move(*candidate);
                                    chosen_strips = plan.stripes.size();
                                    cropped = plan.white.has_value();
                                    converted = variant.converted;
                                    lossy = candidate_lossy;
                                }
                            }
                        }
                        if (form) {
                            commit_image_form(pdf, changes, pdf.getObjectByObjGen(work->target),
                                              std::move(*form));
                            if (size < original_size)
                                progress.bytes_saved += original_size - size;
                            report.images_cropped += cropped;
                            if (chosen_strips > 1) {
                                ++report.images_striped;
                                report.image_strips += chosen_strips;
                            }
                        } else if (best) {
                            if (best_mask) {
                                auto object = pdf.newStream();
                                best->value.replaceKey("/Mask", object);
                                changes.emplace(object.getObjGen(), std::move(*best_mask));
                            }
                            changes.emplace(work->target, std::move(*best));
                            if (size < original_size)
                                progress.bytes_saved += original_size - size;
                        }
                        if (form || best) {
                            if (work->reconstruction) {
                                for (auto& [id, replacement] : work->reconstruction->related)
                                    changes.emplace(id, std::move(replacement));
                                ++report.images_reconstructed;
                            }
                            ++report.images_replaced;
                            ++progress.compressed;
                            report.images_converted_to_bilevel += converted;
                            report.images_lossy += lossy;
                            report.images_forced += !work->original_allowed;
                        } else if (!work->original_allowed)
                            throw std::runtime_error(
                                "image " + object_label(work->source.getObjGen()) +
                                ": cannot satisfy specified-only" +
                                (work->error.empty() ? " with the enabled codecs and PDF version"
                                                     : ": " + work->error));
                        else if (work->error.empty())
                            work->error = "no smaller enabled encoding for this sample layout";
                        if (work->thresholds_skipped)
                            report.notes.push_back("image " +
                                                   object_label(work->source.getObjGen()) +
                                                   ": transparency mask not thresholded to bilevel");
                        if (!work->error.empty())
                            report.notes.push_back("image " +
                                                   object_label(work->source.getObjGen()) + ": " +
                                                   work->error);
                    }
                    ++progress.processed;
                    if (options.progress)
                        options.progress(progress);
                }));
    progress.complete = true;
    if (options.progress)
        options.progress(progress);
}

} // namespace dpdfcomp
