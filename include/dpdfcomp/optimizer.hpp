#pragma once

#include <dpdfcomp/export.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dpdfcomp
{

struct Progress {
    enum class Kind { fonts, images };
    Kind kind;
    std::size_t processed = 0;
    std::size_t total = 0;
    std::size_t compressed = 0;
    std::size_t bytes_saved = 0;
    bool complete = false;
};

enum class ColorCodec { png, lzw, zip, jpeg, jp2_lossless, jp2_lossy, bilevel };

struct ColorEncoding {
    ColorCodec codec;
    int parameter = 0; // PNG iterations, JPEG/JP2 quality, or bilevel threshold.
};

inline constexpr int default_png_iterations = 15;

struct Options {
    bool images = true;
    bool bilevel_flate = true;
    bool recompress_bilevel = true;
    bool bilevel_specified_only = false;
    int bilevel_png_iterations = 0; // Zero disables this candidate.
    bool detect_bilevel = true;
    std::vector<ColorEncoding> color_lossless{{ColorCodec::lzw}, {ColorCodec::zip}};
    std::vector<ColorEncoding> color_lossy;
    bool color_lossless_specified_only = false;
    bool color_lossy_specified_only = false;
    bool ccitt = true;
    bool jbig2 = false;
    int jbig2_stripe_rows = 0; // Zero encodes each image as one generic region.
    int pdf_stripe_rows = 0;   // Zero preserves each PDF image as one object.
    bool reconstruct_strips = false;
    bool crop_internal = false; // Crop white margins before splitting or encoding.
    bool fonts = true;
    bool type1_to_cff = true;
    bool subset_fonts = true;
    bool merge_fonts = true;
    bool upgrade_version = true;
    bool linearize = false; // Linearize even when the input was not linearized.
    int threads = 0;        // Zero lets oneTBB choose.
    bool overwrite = false;
    // Explicit user selections, excluding defaults and expansions of "all".
    // Used only for version-compatibility warnings, not codec selection.
    std::vector<std::string> explicit_codecs;
    std::function<void(const std::string&)> warning;
    // Serialized notifications, possibly on a worker thread. Savings include
    // accepted object replacements; the final Report includes file overhead.
    std::function<void(const Progress&)> progress;
};

struct Report {
    std::size_t input_bytes = 0;
    std::size_t output_bytes = 0;
    std::size_t images_replaced = 0;
    std::size_t images_converted_to_bilevel = 0;
    std::size_t images_lossy = 0;
    std::size_t images_forced = 0; // Codec restrictions required replacement, possibly larger.
    std::size_t images_reconstructed = 0;
    std::size_t images_striped = 0;
    std::size_t images_cropped = 0; // PDF crops; internal JBIG2 cropping keeps the image intact.
    std::size_t image_strips = 0;
    std::size_t fonts_replaced = 0;
    std::size_t fonts_merged = 0;
    bool kept_original = false;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
};

// Writes a verified file atomically without changing the input. Growth is
// permitted for forced codecs, signature removal, or linearization.
// Throws standard C++ exceptions on failure. Call on a GUI worker thread;
// marshal progress/warning callbacks onto the UI thread as needed.
DPDFCOMP_EXPORT Report optimize(const std::filesystem::path& input, const std::filesystem::path& output,
                const Options& options = {});

} // namespace dpdfcomp
