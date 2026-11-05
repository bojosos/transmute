#pragma once

// Drawing PDF pages (CGContextDrawPDFPage, in ../pdf_document.cpp): the
// page's content interpreted over the PDF reader and drawn through the
// context's drawing entry points (so it reaches bitmap and PDF contexts
// alike). See pdf_render.cpp for what is drawn.

#include "alchemy/darwin/coregraphics/context.hpp"
#include "alchemy/darwin/coregraphics/pdf/pdf_parser.hpp"

namespace alchemy::gui::quartz2d {

// Draws `page` of `document` into `c` (locked by the caller) in its current
// user space, clipped to the page's crop box; the graphics state is restored
// afterwards.
void render_pdf_page(Quartz& q, ContextState& c, pdf::Document& document, const pdf::ObjectPtr& page);

} // namespace alchemy::gui::quartz2d
