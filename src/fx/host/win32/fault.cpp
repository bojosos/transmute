// win32 backend of fx/host/fault.hpp: a first vectored exception handler.

#include "fx/host/fault.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdio>

namespace transmute::fx::host {

static_assert(sizeof(FaultRecord) == sizeof(EXCEPTION_RECORD));
static_assert(offsetof(FaultRecord, code) == offsetof(EXCEPTION_RECORD, ExceptionCode));
static_assert(offsetof(FaultRecord, flags) == offsetof(EXCEPTION_RECORD, ExceptionFlags));
static_assert(offsetof(FaultRecord, chained) == offsetof(EXCEPTION_RECORD, ExceptionRecord));
static_assert(offsetof(FaultRecord, address) == offsetof(EXCEPTION_RECORD, ExceptionAddress));
static_assert(offsetof(FaultRecord, parameter_count) == offsetof(EXCEPTION_RECORD, NumberParameters));
static_assert(offsetof(FaultRecord, parameters) == offsetof(EXCEPTION_RECORD, ExceptionInformation));
static_assert(kFaultAccessViolation == EXCEPTION_ACCESS_VIOLATION && kFaultInPageError == EXCEPTION_IN_PAGE_ERROR &&
              kFaultGuardPage == EXCEPTION_GUARD_PAGE && kFaultNoncontinuable == EXCEPTION_NONCONTINUABLE);
static_assert(kFaultDatatypeMisalignment == EXCEPTION_DATATYPE_MISALIGNMENT && kFaultBreakpoint == EXCEPTION_BREAKPOINT &&
              kFaultIllegalInstruction == EXCEPTION_ILLEGAL_INSTRUCTION &&
              kFaultIntegerDivideByZero == EXCEPTION_INT_DIVIDE_BY_ZERO &&
              kFaultIntegerOverflow == EXCEPTION_INT_OVERFLOW &&
              kFaultPrivilegedInstruction == EXCEPTION_PRIV_INSTRUCTION && kFaultStackOverflow == EXCEPTION_STACK_OVERFLOW);
static_assert(kFaultRead == 0 && kFaultWrite == EXCEPTION_WRITE_FAULT && kFaultExecute == EXCEPTION_EXECUTE_FAULT);
static_assert(win32_context::kEFlags == offsetof(CONTEXT, EFlags) && win32_context::kRax == offsetof(CONTEXT, Rax) &&
              win32_context::kRsp == offsetof(CONTEXT, Rsp) && win32_context::kRip == offsetof(CONTEXT, Rip) &&
              win32_context::kXmm0 == offsetof(CONTEXT, Xmm0));
static_assert(offsetof(CONTEXT, R15) == win32_context::kRax + 15 * 8 && sizeof(M128A) == 16);

namespace {

FaultHandler g_handler = nullptr;

LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* pointers) {
    const FaultAction action = g_handler(*reinterpret_cast<FaultRecord*>(pointers->ExceptionRecord),
                                         *reinterpret_cast<CpuContext*>(pointers->ContextRecord));
    return action == FaultAction::resume ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool install_fault_handler(FaultHandler handler) noexcept {
    g_handler = handler;
    return AddVectoredExceptionHandler(1, &vectored_handler) != nullptr;
}

void trace_fault(const FaultRecord& record, const CpuContext& context, bool (*annotate)(std::uint64_t pc)) noexcept {
    char line[160];
    const int n = wsprintfA(line, "fx veh: code %08lx rip %p info %p %p\n", record.code,
                            reinterpret_cast<void*>(rip(context)),
                            reinterpret_cast<void*>(record.parameter_count > 0 ? record.parameters[0] : 0),
                            reinterpret_cast<void*>(record.parameter_count > 1 ? record.parameters[1] : 0));
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
    if (record.code != EXCEPTION_ACCESS_VIOLATION) return;
    void* frames[24];
    const auto count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = name;
        for (const char* p = name; *p; ++p) if (*p == '\\') base_name = p + 1;
        const auto offset = module ? reinterpret_cast<std::uint64_t>(frames[i]) - reinterpret_cast<std::uint64_t>(module)
                                   : 0;
        const bool marked = annotate && annotate(reinterpret_cast<std::uint64_t>(frames[i]));
        const int m = std::snprintf(line, sizeof(line), "    #%u %p %s+0x%llx%s\n", i, frames[i], base_name,
                                    static_cast<unsigned long long>(offset), marked ? " (code arena)" : "");
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(m), &written, nullptr);
    }
}

} // namespace transmute::fx::host
