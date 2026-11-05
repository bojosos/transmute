#pragma once

// Printing over the Quartz layer (appkit_printing.cpp): NSPrintOperation,
// NSPrinter, NSPDFImageRep and NSView's printing methods, installed by
// Quartz::install_appkit_printing.

#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

namespace alchemy::gui::quartz2d {

// A +1 CGImage of `image`'s NSPDFImageRep (its current page at the image's
// size, `scale` pixels per point); 0 when the image holds none (the NSImage
// drawing in appkit_drawing.cpp falls back to it).
Address render_pdf_rep_image(Quartz& q, Address image, double scale);

} // namespace alchemy::gui::quartz2d
