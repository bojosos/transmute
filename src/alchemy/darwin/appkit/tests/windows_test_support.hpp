#pragma once

// Shared helpers of the QLab gap group G4 host-only tests.
#include <cmath>
#include <stdexcept>
#include <string>

namespace windows_tests {

inline void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
inline bool near(double a, double b, double tolerance = 1.0e-6) { return std::fabs(a - b) <= tolerance; }

void solver_tests();     // layout_solver_tests.cpp
void geometry_tests();   // layout_geometry_tests.cpp

} // namespace windows_tests
