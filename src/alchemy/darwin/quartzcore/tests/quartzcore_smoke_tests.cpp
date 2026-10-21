// QuartzCore smoke test (owner: A7): the contracts compile together and the
// pure helpers behave without a device, a window server or Vulkan headers.
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/window_server.hpp"
#include "alchemy/darwin/quartzcore/presenter.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"

#include <iostream>

int main() {
    namespace qc = alchemy::gui::quartzcore;
    qc::LayerHost host;
    host.frame = {{0, 0}, {640, 480}};
    if (host.backing_scale != 1 || host.native_window || alchemy::gui::window_server_available()) {
        std::cerr << "QuartzCore contract expectations changed\n";
        return 1;
    }
    if (qc::layer_pixel_format_problem(qc::pixel_format::bgra8_unorm) ||
        !qc::layer_pixel_format_problem(qc::pixel_format::bgr10_xr)) {
        std::cerr << "CAMetalLayer pixel format table is wrong\n";
        return 1;
    }
    std::cout << "Alchemy GUI QuartzCore smoke passed\n";
    return 0;
}
