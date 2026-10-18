#include <dpdfcomp/optimizer.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{
constexpr std::string_view help = R"(dpdfcomp 1.0 - PDF font and image compression

Usage: dpdfcomp [options] input.pdf output.pdf

  --no-fonts             Do not compress any fonts
  --type1-to-cff         Enable Type 1 conversion (default)
  --no-type1-to-cff      Do not compress Type 1 fonts
  --no-font-subset       Do not subset embedded fonts
  --no-font-merge        Do not merge embedded fonts
  --no-images            Do not compress images
  --bilevel=LIST         Set codecs for bilevel images (default: flate,ccitt)
  --no-bilevel           Do not recompress existing bilevel images
  --detect-bilevel       Detect bilevel images encoded as color (default)
  --no-detect-bilevel    Disable automatic bilevel detection
  --color-lossless=LIST  Set codecs for lossless color images (default: lzw,zip)
  --no-color-lossless    Disable compression for originally lossless color images
  --color-lossy=LIST     Set codecs for lossy color images (default: none)
  --no-color-lossy       Disable compression for originally lossy color images
  --jbig2-stripe=X       Encode JBIG2 images using stripes of height X; 0 disables
  --pdf-stripe=X         Split images into stripes of height X; 0 disables (default).
  --reconstruct-stripes  Merge stripes back into single image
  --linearize            Linearize output PDF
  --crop-internal        Crop white margins before striping and compression
  --upgrade              Allow newer codecs to raise the PDF version (default)
  --no-upgrade           Restrict codecs to the input PDF version
  --threads=X            Current threads; 0 (default) sets number automatically
  --force                Replace an existing output file
  --verbose              Notify when objects are copied without further compression
  --no-progress          Show only warnings and errors
  --silent               Produce no command-line output
  --help                 Show this help
  --version              Show the program version

Color codecs: png[-X],lzw,zip,jpeg[-XX],jp2-lossless,jp2-lossy[-XX],bilevel[-XX],all,none
Bilevel codecs: flate,ccitt,jbig2,png[-X],all,none

Add specified-only to allow only specified output codecs, even if bigger than the original.

all results in using all available lossless codecs.

png runs ZopfliPNG with X iterations (default: 15).
jpeg quality is 0-100 (default: 75). Lossy JP2 0-100 (default: 75) targets PSNR 20-60 dB.
Bilevel thresholds are 0-100% (default: 50).

PDF strips/crops preserve samples and placement; viewer sampling can change.

Encrypted PDFs are not supported. Signed PDFs produce unsigned output.

Use -- to end option parsing.
)";

int number(std::string_view value, int minimum, int maximum, std::string_view name)
{
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < minimum ||
        result > maximum)
        throw std::runtime_error(std::string(name) + " must be between " + std::to_string(minimum) +
                                 " and " + std::to_string(maximum));
    return result;
}

std::vector<std::string_view> list_items(std::string_view list)
{
    std::vector<std::string_view> result;
    if (list == "none" || list.empty())
        return result;
    while (true) {
        const auto comma = list.find(',');
        const auto item = list.substr(0, comma);
        if (item.empty())
            throw std::runtime_error("empty codec in list");
        result.push_back(item);
        if (comma == std::string_view::npos)
            return result;
        list.remove_prefix(comma + 1);
    }
}

int png_iterations(std::string_view value)
{
    return value == "png"
               ? dpdfcomp::default_png_iterations
               : number(value.substr(4), 1, std::numeric_limits<int>::max(), "PNG iterations");
}

std::vector<std::string> explicit_codecs(std::string_view list)
{
    std::vector<std::string> result;
    for (auto item : list_items(list))
        if (item != "all" && item != "specified-only")
            result.emplace_back(item);
    return result;
}

std::vector<dpdfcomp::ColorEncoding> color_encodings(std::string_view list, bool& specified_only)
{
    using Codec = dpdfcomp::ColorCodec;
    std::vector<dpdfcomp::ColorEncoding> result;
    specified_only = false;
    for (auto item : list_items(list)) {
        if (item == "specified-only")
            specified_only = true;
        else if (item == "all") {
            bool ignored = false;
            auto all = color_encodings("png,lzw,zip,jp2-lossless", ignored);
            result.insert(result.end(), all.begin(), all.end());
        } else if (item == "png" || item.starts_with("png-"))
            result.push_back({Codec::png, png_iterations(item)});
        else if (item == "lzw")
            result.push_back({Codec::lzw});
        else if (item == "zip")
            result.push_back({Codec::zip});
        else if (item == "jp2-lossless")
            result.push_back({Codec::jp2_lossless});
        else if (item == "bilevel")
            result.push_back({Codec::bilevel, 50});
        else if (item.starts_with("bilevel-"))
            result.push_back({Codec::bilevel, number(item.substr(8), 0, 100, "bilevel threshold")});
        else if (item == "jpeg" || item.starts_with("jpeg-"))
            result.push_back({Codec::jpeg, item == "jpeg"
                                               ? 75
                                               : number(item.substr(5), 0, 100, "JPEG quality")});
        else if (item == "jp2-lossy" || item.starts_with("jp2-lossy-"))
            result.push_back(
                {Codec::jp2_lossy,
                 item == "jp2-lossy" ? 75 : number(item.substr(10), 0, 100, "JPEG 2000 quality")});
        else
            throw std::runtime_error("unknown color codec: " + std::string(item));
    }
    std::vector<dpdfcomp::ColorEncoding> unique;
    for (auto candidate : result)
        if (std::none_of(unique.begin(), unique.end(), [&](auto other) {
                return candidate.codec == other.codec && candidate.parameter == other.parameter;
            }))
            unique.push_back(candidate);
    if (specified_only && unique.empty())
        throw std::runtime_error("specified-only requires at least one codec");
    return unique;
}

void bilevel_encodings(dpdfcomp::Options& options, std::string_view list)
{
    options.recompress_bilevel = true;
    options.bilevel_specified_only = false;
    options.ccitt = options.jbig2 = options.bilevel_flate = false;
    options.bilevel_png_iterations = 0;
    for (auto item : list_items(list)) {
        if (item == "specified-only")
            options.bilevel_specified_only = true;
        else if (item == "all") {
            options.bilevel_flate = options.ccitt = options.jbig2 = true;
            options.bilevel_png_iterations =
                std::max(options.bilevel_png_iterations, dpdfcomp::default_png_iterations);
        } else if (item == "flate")
            options.bilevel_flate = true;
        else if (item == "ccitt")
            options.ccitt = true;
        else if (item == "jbig2")
            options.jbig2 = true;
        else if (item == "png" || item.starts_with("png-"))
            options.bilevel_png_iterations =
                std::max(options.bilevel_png_iterations, png_iterations(item));
        else
            throw std::runtime_error("unknown bilevel codec: " + std::string(item));
    }
}

class ConsoleProgress
{
  public:
    ~ConsoleProgress()
    {
        if (line_open_)
            std::cerr << '\n';
    }

    void operator()(const dpdfcomp::Progress& progress)
    {
        using Clock = std::chrono::steady_clock;
        auto now = Clock::now();
        bool first = kind_ != progress.kind;
        if (!progress.complete &&
            ((!first && now - last_ < (interactive_ ? std::chrono::milliseconds(100)
                                                    : std::chrono::milliseconds(2000))) ||
             progress.total == 0))
            return;
        kind_ = progress.kind;
        last_ = now;
        std::ostringstream line;
        line << progress.compressed << '/' << progress.total
             << (progress.kind == dpdfcomp::Progress::Kind::fonts ? " font" : " image")
             << " objects compressed (" << std::fixed << std::setprecision(1)
             << progress.bytes_saved / 1'000'000.0 << " MB total savings so far)";
        if (progress.complete && progress.compressed < progress.total)
            line << "; " << progress.total - progress.compressed << " preserved";
        if (interactive_)
            std::cerr << '\r' << "\033[K";
        std::cerr << line.str();
        line_open_ = interactive_ && !progress.complete;
        if (!line_open_)
            std::cerr << '\n';
        std::cerr.flush();
    }

  private:
#ifdef _WIN32
    bool interactive_ = false; // Plain progress also works in legacy Windows consoles.
#else
    bool interactive_ = isatty(STDERR_FILENO);
#endif
    bool line_open_ = false;
    std::optional<dpdfcomp::Progress::Kind> kind_;
    std::chrono::steady_clock::time_point last_;
};
} // namespace

int run(int argc, char** argv)
{
    // Scan before parsing so silence also covers help, parse errors, and C
    // library diagnostics. Arguments after -- are filenames, not options.
    bool silent = false, quiet = false;
    for (int i = 1; i < argc && std::string_view(argv[i]) != "--"; ++i) {
        silent |= std::string_view(argv[i]) == "--silent";
        quiet |= std::string_view(argv[i]) == "--no-progress";
    }
    quiet |= silent;
    if (silent) {
#ifdef _WIN32
        constexpr auto null_device = "NUL";
#else
        constexpr auto null_device = "/dev/null";
#endif
        if (!std::freopen(null_device, "w", stderr) || !std::freopen(null_device, "w", stdout))
            return 1;
    }
    try {
        dpdfcomp::Options options;
        bool verbose = false, positional = false;
        bool jbig2_stripe_specified = false;
        std::vector<std::string> paths;
        std::array<std::vector<std::string>, 3> selections;
        for (int i = 1; i < argc; ++i) {
            std::string_view arg(argv[i]);
            if (positional || !arg.starts_with("-")) {
                paths.emplace_back(arg);
                continue;
            }
            if (arg == "--")
                positional = true;
            else if (arg == "--help" || arg == "-h") {
                if (!quiet)
                    std::cout << help;
                return 0;
            } else if (arg == "--version") {
                if (!quiet)
                    std::cout << "dpdfcomp 1.0\n";
                return 0;
            } else if (arg == "--no-fonts")
                options.fonts = false;
            else if (arg == "--type1-to-cff")
                options.type1_to_cff = true;
            else if (arg == "--no-type1-to-cff")
                options.type1_to_cff = false;
            else if (arg == "--no-font-subset")
                options.subset_fonts = false;
            else if (arg == "--no-font-merge")
                options.merge_fonts = false;
            else if (arg == "--no-images")
                options.images = false;
            else if (arg == "--no-bilevel") {
                options.recompress_bilevel = false;
                selections[0].clear();
            } else if (arg == "--detect-bilevel")
                options.detect_bilevel = true;
            else if (arg == "--no-detect-bilevel")
                options.detect_bilevel = false;
            else if (arg == "--no-color-lossless") {
                options.color_lossless.clear();
                options.color_lossless_specified_only = false;
                selections[1].clear();
            } else if (arg == "--no-color-lossy") {
                options.color_lossy.clear();
                options.color_lossy_specified_only = false;
                selections[2].clear();
            } else if (arg.starts_with("--color-lossless=")) {
                options.color_lossless =
                    color_encodings(arg.substr(17), options.color_lossless_specified_only);
                selections[1] = explicit_codecs(arg.substr(17));
            } else if (arg.starts_with("--color-lossy=")) {
                options.color_lossy =
                    color_encodings(arg.substr(14), options.color_lossy_specified_only);
                selections[2] = explicit_codecs(arg.substr(14));
            } else if (arg == "--reconstruct-stripes")
                options.reconstruct_strips = true;
            else if (arg == "--linearize")
                options.linearize = true;
            else if (arg == "--crop-internal")
                options.crop_internal = true;
            else if (arg == "--upgrade")
                options.upgrade_version = true;
            else if (arg == "--no-upgrade")
                options.upgrade_version = false;
            else if (arg == "--force")
                options.overwrite = true;
            else if (arg == "--verbose")
                verbose = true;
            else if (arg == "--silent" || arg == "--no-progress")
                quiet = true;
            else if (arg.starts_with("--threads=")) {
                auto value = arg.substr(10);
                auto [end, error] =
                    std::from_chars(value.data(), value.data() + value.size(), options.threads);
                if (error != std::errc{} || end != value.data() + value.size() ||
                    options.threads < 0 || options.threads > 4096)
                    throw std::runtime_error("--threads must be between 0 and 4096");
            } else if (arg.starts_with("--jbig2-stripe=") || arg.starts_with("--pdf-stripe=")) {
                jbig2_stripe_specified |= arg.starts_with("--jbig2-stripe=");
                const auto split = arg.find('=');
                auto value = arg.substr(split + 1);
                auto& rows =
                    arg.starts_with("--pdf-") ? options.pdf_stripe_rows : options.jbig2_stripe_rows;
                auto [end, error] =
                    std::from_chars(value.data(), value.data() + value.size(), rows);
                if (error != std::errc{} || end != value.data() + value.size() || rows < 0)
                    throw std::runtime_error(std::string(arg.substr(0, split)) +
                                             " must be a nonnegative integer");
            } else if (arg.starts_with("--bilevel=")) {
                bilevel_encodings(options, arg.substr(10));
                selections[0] = explicit_codecs(arg.substr(10));
            } else
                throw std::runtime_error("unknown option: " + std::string(arg));
        }
        if (paths.size() != 2) {
            if (quiet)
                std::cerr << "dpdfcomp: expected input.pdf and output.pdf\n";
            else
                std::cerr << help;
            return 2;
        }
        ConsoleProgress progress;
        for (const auto& selection : selections)
            options.explicit_codecs.insert(options.explicit_codecs.end(), selection.begin(),
                                           selection.end());
        options.warning = [](const std::string& warning) {
            std::cerr << "Warning: " << warning << '\n';
        };
        if (jbig2_stripe_specified && (!options.jbig2 || !options.recompress_bilevel))
            options.warning("JBIG2 stripe height specified, but JBIG2 is not one of the specified "
                            "output codecs.");
        if (!quiet)
            options.progress = std::ref(progress);
        auto report = dpdfcomp::optimize(std::u8string(paths[0].begin(), paths[0].end()),
                                         std::u8string(paths[1].begin(), paths[1].end()), options);
        if (quiet)
            return 0;
        std::cout << report.input_bytes << " -> " << report.output_bytes << " bytes; "
                  << report.images_replaced << " images, " << report.fonts_replaced << " fonts, "
                  << report.fonts_merged << " merges\n";
        if (report.images_converted_to_bilevel)
            std::cout << report.images_converted_to_bilevel << " images converted to bilevel.\n";
        if (report.images_forced)
            std::cout << report.images_forced
                      << " images replaced to satisfy codec restrictions.\n";
        if (report.images_lossy)
            std::cout << report.images_lossy
                      << " images used explicitly requested lossy compression.\n";
        if (report.images_reconstructed)
            std::cout << report.images_reconstructed
                      << " groups of strips reconstructed as images.\n";
        if (report.images_striped)
            std::cout << report.images_striped << " images split into " << report.image_strips
                      << " PDF strips.\n";
        if (report.images_cropped)
            std::cout << report.images_cropped
                      << " images cropped within their original placement.\n";
        if (report.kept_original)
            std::cout << "Kept original bytes: no smaller verified result.\n";
        if (verbose)
            for (const auto& note : report.notes)
                std::cerr << note << '\n';
        else if (!report.notes.empty())
            std::cerr << report.notes.size() << " notes (use --verbose).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dpdfcomp: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    bool silent = false;
    for (int i = 1; i < argc && std::wstring_view(argv[i]) != L"--"; ++i)
        silent |= std::wstring_view(argv[i]) == L"--silent";
    try {
        std::vector<std::string> arguments;
        arguments.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            const auto utf8 = std::filesystem::path(argv[i]).u8string();
            arguments.emplace_back(utf8.begin(), utf8.end());
        }
        std::vector<char*> pointers;
        pointers.reserve(argc + 1);
        for (auto& argument : arguments)
            pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        return run(argc, pointers.data());
    } catch (const std::exception& error) {
        if (!silent)
            std::cerr << "dpdfcomp: " << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char** argv)
{
    return run(argc, argv);
}
#endif
