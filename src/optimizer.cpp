#include <dpdfcomp/optimizer.hpp>

#include "fonts.hpp"
#include "images.hpp"
#include "pdf.hpp"
#include <oneapi/tbb/task_arena.h>

#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <random>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dpdfcomp
{
namespace
{

class TemporaryFile
{
  public:
    explicit TemporaryFile(const std::filesystem::path& destination)
    {
#ifdef _WIN32
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            path_ = destination.parent_path() /
                    (L".dpdfcomp-" + std::to_wstring(random()) + L"-" + std::to_wstring(random()));
            auto file = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
                return;
            }
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                throw std::system_error(error, std::system_category(),
                                        "create output temporary file");
        }
        throw std::runtime_error("cannot create a unique output temporary file");
#else
        auto pattern = (destination.parent_path() / ".dpdfcomp-XXXXXX").string();
        int fd = mkstemp(pattern.data());
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "create output temporary file");
        close(fd);
        path_ = std::move(pattern);
#endif
    }
    ~TemporaryFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

} // namespace

Report optimize(const std::filesystem::path& input, const std::filesystem::path& output,
                const Options& options)
{
    namespace fs = std::filesystem;
    if (options.threads < 0 || options.threads > 4096)
        throw std::invalid_argument("thread count must be between 0 and 4096");
    if (options.jbig2_stripe_rows < 0)
        throw std::invalid_argument("JBIG2 stripe height must be nonnegative");
    if (options.pdf_stripe_rows < 0)
        throw std::invalid_argument("PDF stripe height must be nonnegative");
    if (options.bilevel_png_iterations < 0)
        throw std::invalid_argument(
            "PNG iterations must be positive, or zero to disable the bilevel candidate");
    for (const auto* codecs : {&options.color_lossless, &options.color_lossy})
        for (auto encoding : *codecs) {
            if (encoding.codec == ColorCodec::png && encoding.parameter <= 0)
                throw std::invalid_argument("PNG iterations must be positive");
            if ((encoding.codec == ColorCodec::jpeg || encoding.codec == ColorCodec::jp2_lossy ||
                 encoding.codec == ColorCodec::bilevel) &&
                (encoding.parameter < 0 || encoding.parameter > 100))
                throw std::invalid_argument("image quality or threshold must be between 0 and 100");
        }
    if (options.bilevel_specified_only && !options.bilevel_flate && !options.ccitt &&
        !options.jbig2 && !options.bilevel_png_iterations)
        throw std::invalid_argument("specified-only requires at least one bilevel codec");
    if ((options.color_lossless_specified_only && options.color_lossless.empty()) ||
        (options.color_lossy_specified_only && options.color_lossy.empty()))
        throw std::invalid_argument("specified-only requires at least one color codec");
    if (!fs::is_regular_file(input))
        throw std::runtime_error("input is not a regular file");
    if (fs::exists(output) && fs::equivalent(input, output))
        throw std::runtime_error("input and output must be different files");
    if (fs::exists(output) && !options.overwrite)
        throw std::runtime_error("output exists; use --force to replace it");
    QPDF pdf;
    pdf.setAttemptRecovery(false);
    pdf.setSuppressWarnings(true);
    pdf.processFile(utf8_path(input).c_str());
    Report report;
    report.warnings = check_document(pdf);
    if (options.warning)
        for (const auto& warning : report.warnings)
            options.warning(warning);
    report.input_bytes = fs::file_size(input);
    const bool linearized = options.linearize || pdf.isLinearized();
    Replacements changes;
    oneapi::tbb::task_arena arena(options.threads ? options.threads
                                                  : oneapi::tbb::task_arena::automatic);
    arena.execute([&] {
        if (options.images)
            optimize_images(pdf, changes, options, report);
        if (options.fonts)
            optimize_fonts(pdf, changes, options, report);
    });
    const bool signed_input = remove_signatures(pdf, changes);
    if (signed_input) {
        const std::string warning =
            "input PDF was signed, but compressed output PDF is not signed.";
        report.warnings.push_back(warning);
        if (options.warning)
            options.warning(warning);
    }
    if (pdf.anyWarnings())
        throw std::runtime_error("input warnings encountered; output not written");
    TemporaryFile temporary(output);
    const bool must_rewrite = signed_input || linearized;
    if (!changes.empty() || must_rewrite) {
        write_pdf(pdf, changes, temporary.path());
        verify_pdf(pdf, changes, temporary.path());
        if (linearized) {
            TemporaryFile linearized_output(output);
            linearize_pdf(temporary.path(), linearized_output.path());
            fs::copy_file(linearized_output.path(), temporary.path(),
                          fs::copy_options::overwrite_existing);
        }
    }
    if (!must_rewrite &&
        (changes.empty() ||
         (!report.images_forced && fs::file_size(temporary.path()) >= report.input_bytes))) {
        fs::copy_file(input, temporary.path(), fs::copy_options::overwrite_existing);
        report.kept_original = true;
        report.images_replaced = report.fonts_replaced = report.fonts_merged = 0;
        report.images_striped = report.image_strips = 0;
        report.images_reconstructed = report.images_cropped = report.images_converted_to_bilevel =
            report.images_lossy = report.images_forced = 0;
    }
    report.output_bytes = fs::file_size(temporary.path());
#ifdef _WIN32
    if (!MoveFileExW(temporary.path().c_str(), output.c_str(),
                     options.overwrite ? MOVEFILE_REPLACE_EXISTING : 0))
        throw std::system_error(GetLastError(), std::system_category(), "publish output file");
#else
    if (options.overwrite) {
        fs::rename(temporary.path(), output);
    } else {
        // Same-directory publication is atomic and cannot overwrite a file
        // created since the initial check. The destructor removes our link.
        fs::create_hard_link(temporary.path(), output);
    }
#endif
    return report;
}

} // namespace dpdfcomp
