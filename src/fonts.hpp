#pragma once
#include "pdf.hpp"
#include <dpdfcomp/optimizer.hpp>

namespace dpdfcomp
{
void optimize_fonts(QPDF&, Replacements&, const Options&, Report&);
}
