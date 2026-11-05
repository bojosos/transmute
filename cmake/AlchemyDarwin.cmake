# macOS frameworks, one directory each (src/alchemy/darwin/<framework>/) and
# one fragment each (cmake/darwin/<framework>.cmake), owned by the framework.
# Included at the end of cmake/Alchemy.cmake, after cmake/AlchemyGui.cmake, so
# a fragment may use everything AlchemyGui.cmake defines. The pattern is
# documented in docs/ALCHEMY_PROVIDERS.md.
#
# A fragment
#   - adds the framework's headless sources (part of alchemy.exe and of
#     alchemy-fx) with target_sources(alchemy_core PRIVATE ...);
#   - appends its GUI sources (alchemy-fx only) to ALCHEMY_GUI_SOURCES, and may
#     append to ALCHEMY_GUI_INCLUDE_DIRECTORIES, ALCHEMY_GUI_COMPILE_DEFINITIONS
#     and ALCHEMY_GUI_LINK_LIBRARIES, as a cmake/gui/ fragment does;
#   - lists, in ALCHEMY_HEADLESS_ONLY_SOURCES, the headless shells that only
#     alchemy.exe needs: a file (<name>_headless.cpp by convention) that
#     registers names the framework's GUI provider owns in alchemy-fx. It is
#     part of alchemy_core but not of alchemy_core_fx, so each name has one
#     owner in each build (cmake/AlchemyFx.cmake removes these files);
#   - registers its tests: headless ones directly (add_executable + add_test,
#     names "alchemy_<framework>_..."), GUI ones through a function passed to
#     alchemy_gui_register_tests() (see cmake/AlchemyGui.cmake).
# Fragments are included in name order; they only append, so the order does
# not matter.
set(ALCHEMY_HEADLESS_ONLY_SOURCES)
# The headless framework set's sequence (both builds): darwin/frameworks.cpp
# (install_darwin_frameworks) and darwin/headless_additions.cpp; the pieces
# they call are each framework's own sources.
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/frameworks.cpp
    src/alchemy/darwin/headless_additions.cpp)
# Local test inputs that never enter git (a translator DLL, dumped shader
# corpora) live under artifacts/. A git worktree has no artifacts/ of its own,
# so alchemy_local_artifact(<out> <relative path>) looks in this checkout's
# artifacts/, then in the main checkout's (the parent of git's common
# directory); <out> is the first that exists, else empty.
set(alchemy_artifact_roots "${PROJECT_SOURCE_DIR}/artifacts")
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse --path-format=absolute --git-common-dir
        OUTPUT_VARIABLE alchemy_git_common_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE alchemy_git_result)
    if(alchemy_git_result EQUAL 0 AND alchemy_git_common_dir)
        get_filename_component(alchemy_main_checkout "${alchemy_git_common_dir}" DIRECTORY)
        if(NOT alchemy_main_checkout STREQUAL PROJECT_SOURCE_DIR)
            list(APPEND alchemy_artifact_roots "${alchemy_main_checkout}/artifacts")
        endif()
    endif()
endif()
function(alchemy_local_artifact out relative)
    foreach(root IN LISTS alchemy_artifact_roots)
        if(EXISTS "${root}/${relative}")
            set(${out} "${root}/${relative}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} "" PARENT_SCOPE)
endfunction()

file(GLOB alchemy_darwin_fragments CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/darwin/*.cmake")
list(SORT alchemy_darwin_fragments)
foreach(fragment IN LISTS alchemy_darwin_fragments)
    include("${fragment}")
endforeach()
