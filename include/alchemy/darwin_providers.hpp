#pragma once

// The macOS layer of one Alchemy process: every provider, installed in the one
// fixed order that run_program, `alchemy doctor` and the provider tests share.
//
//   DarwinServices          libSystem core (files, sockets, processes, signals)
//   install_darwin_mach_vm  Mach VM
//   install_darwin_native   libSystem natives
//   install_darwin_numeric  libm and the numeric natives
//   install_darwin_dispatch libdispatch
//   DarwinThreads           pthreads and dispatch workers
//   ObjcRuntime             libobjc
//   CoreFoundation          CoreFoundation value types
//   FoundationBridge        Foundation value bridge
//   install_darwin_frameworks  headless framework identities and shells
//   install_darwin_gui         the GUI providers (alchemy-fx only, ALCHEMY_WITH_GUI;
//                              include/alchemy/gui/install.hpp)
//
// Then, with ALCHEMY_REGISTRATION_AUDIT set, the registration audit reports.
// The members are constructed in that order and destroyed in reverse, exactly
// as the stack objects they replace were. Not copyable or movable: the
// providers hold references to each other and to the context.

#include "alchemy/core_foundation.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/objc_runtime.hpp"

namespace alchemy {

class ProcessContext;

class DarwinProviders {
public:
    explicit DarwinProviders(ProcessContext& context);
    DarwinProviders(const DarwinProviders&) = delete;
    DarwinProviders& operator=(const DarwinProviders&) = delete;

    DarwinServices services;

private:
    [[maybe_unused]] bool core_installed_; // mach_vm, native, numeric, dispatch (between services and threads)

public:
    DarwinThreads threads;
    ObjcRuntime objc;
    CoreFoundation cf;
    FoundationBridge foundation;

private:
    [[maybe_unused]] bool frameworks_installed_; // frameworks, GUI, audit report
};

} // namespace alchemy
