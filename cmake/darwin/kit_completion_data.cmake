# QLab gap group G1 (data): Foundation data and serialization (archivers, NSData/NSString/collections additions, scanners, predicates, decimal numbers, calendars, formatters, UTType).
# Owner: the G1 implementation agent (docs/ALCHEMY_QLAB_GAPS.md). Only this
# group edits this file and src/alchemy/darwin/kit/completion/data/.
set(alchemy_completion_data_cores)
list(APPEND ALCHEMY_GUI_SOURCES
    ${alchemy_completion_data_cores}
    src/alchemy/darwin/kit/completion/data/install.cpp)
# The data tests moved to their frameworks: alchemy_gui_foundation_data_unit,
# alchemy_gui_coreservices_uttype and alchemy_gui_compression_unit.
