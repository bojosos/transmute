#pragma once

// One framework's test case in the shared executable alchemy_gui_framework_cases.
// A framework with a small provider registers its case from its own tests/
// directory instead of linking one more test executable:
//   cmake/darwin/<framework>.cmake: alchemy_gui_add_framework_case(<name> <source>)
//   <source>:                       const fill_test::Case <name>_case("<name>", [](Fixture& f) {...});
// ctest runs each case as alchemy_gui_<name> (FILL_TEST_ONLY=<name>, in its own
// process) and all of them together as alchemy_gui_framework_cases.

#include "alchemy/gui/tests/fill_fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fill_test {

using Cases = std::vector<std::pair<const char*, std::function<void(Fixture&)>>>;

inline Cases& registered_cases() {
    static Cases cases;
    return cases;
}

struct Case {
    Case(const char* name, std::function<void(Fixture&)> test) { registered_cases().emplace_back(name, std::move(test)); }
};

// Runs the registered cases, by name. FILL_TEST_ONLY must name registered
// cases: a case that does not exist would otherwise pass without running.
inline int run_cases(std::string_view suite) {
    auto cases = registered_cases();
    std::sort(cases.begin(), cases.end(), [](const auto& a, const auto& b) { return std::string_view(a.first) < b.first; });
    if (const char* only = std::getenv("FILL_TEST_ONLY")) {
        std::string list(only);
        for (std::size_t start = 0; start <= list.size();) {
            const auto end = std::min(list.find(',', start), list.size());
            const auto name = std::string_view(list).substr(start, end - start);
            const bool known = std::any_of(cases.begin(), cases.end(), [&](const auto& item) { return name == item.first; });
            if (!name.empty() && !known) {
                std::cerr << "FAILED: no framework case named " << name << '\n';
                return 1;
            }
            start = end + 1;
        }
    }
    return run(suite, cases);
}

} // namespace fill_test
