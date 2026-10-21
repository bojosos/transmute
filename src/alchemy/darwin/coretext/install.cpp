#include "alchemy/darwin/coretext/ct_internal.hpp"

// CoreText installation: fonts, attributed strings and paragraph styles,
// then the typesetter. Nothing touches DirectWrite until a guest asks for a
// font (alchemy-fx doctor and headless runs install too).
namespace alchemy::gui::quartz2d {

void Quartz::install_coretext() {
    coretext() = {};
    install_font_exports(*this);
    install_attributed_strings(*this);
    install_typesetter(*this);
}

} // namespace alchemy::gui::quartz2d
