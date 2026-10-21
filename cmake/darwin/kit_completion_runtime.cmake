# QLab gap group G2 (runtime): Foundation runtime and system (exceptions, locks, invocations, operations, notification queues, undo, KVO/bindings controllers, tasks/pipes/file handles, scripting, XPC, os_log store, dispatch objects).
# Owner: the G2 implementation agent (docs/ALCHEMY_QLAB_GAPS.md). Only this
# group edits this file and src/alchemy/darwin/kit/completion/runtime/.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/kit/completion/runtime/common.cpp
    src/alchemy/darwin/kit/completion/runtime/install.cpp)

# The runtime group's tests moved to their frameworks (darwin/foundation/tests,
# darwin/libsystem/tests, darwin/dispatch/tests); runtime_test_kit.hpp is the
# shared fixture.
