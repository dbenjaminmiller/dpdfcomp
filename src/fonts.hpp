#pragma once
#include "pdf.hpp"
#include <dpdfcomp/optimizer.hpp>

namespace dpdfcomp
{
class FontUsage;
void optimize_cid_fonts(QPDF& pdf, Replacements& changes, const Options& options, Report& report,
                        const FontUsage& usage, Progress& progress);
void optimize_truetype_fonts(QPDF& pdf, Replacements& changes, const Options& options,
                             Report& report, const FontUsage& usage, Progress& progress);
void optimize_fonts(QPDF&, Replacements&, const Options&, Report&);
} // namespace dpdfcomp
