#pragma once

// The winnt.h structures of the Windows ARM64 exception model that fx/seh
// uses as guest layouts, for hosts without the Windows SDK. They follow the
// documented ARM64 ABI (exception records, ARM64_NT_CONTEXT, .pdata/.xdata
// runtime function entries, dispatcher context); seh.hpp's static_asserts
// check the sizes, and on Windows seh.hpp takes the SDK's own definitions.
// Guest and host are both LP64, so pointer members have the guest's size.

#include <cstdint>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"   // anonymous structs in unions, as winnt.h has them
#endif

using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = std::uint32_t;
using LONG = std::int32_t;
using ULONG = std::uint32_t;
using LONGLONG = std::int64_t;
using ULONGLONG = std::uint64_t;
using DWORD64 = std::uint64_t;
using ULONG_PTR = std::uintptr_t;
using BOOLEAN = std::uint8_t;
using PVOID = void*;
using PBYTE = BYTE*;
using PDWORD64 = DWORD64*;

inline constexpr DWORD EXCEPTION_MAXIMUM_PARAMETERS = 15;
inline constexpr LONG EXCEPTION_EXECUTE_HANDLER = 1;
inline constexpr LONG EXCEPTION_CONTINUE_SEARCH = 0;
inline constexpr LONG EXCEPTION_CONTINUE_EXECUTION = -1;
inline constexpr DWORD EXCEPTION_ACCESS_VIOLATION = 0xC0000005u;
inline constexpr DWORD EXCEPTION_IN_PAGE_ERROR = 0xC0000006u;
inline constexpr DWORD EXCEPTION_GUARD_PAGE = 0x80000001u;
inline constexpr DWORD EXCEPTION_DATATYPE_MISALIGNMENT = 0x80000002u;

struct EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
};
using PEXCEPTION_RECORD = EXCEPTION_RECORD*;

union ARM64_NT_NEON128 {
    struct {
        ULONGLONG Low;
        LONGLONG High;
    };
    double D[2];
    float S[4];
    WORD H[8];
    BYTE B[16];
};

struct ARM64_NT_CONTEXT {
    DWORD ContextFlags;
    DWORD Cpsr;
    union {
        struct {
            DWORD64 X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15, X16, X17, X18, X19, X20,
                X21, X22, X23, X24, X25, X26, X27, X28;
            DWORD64 Fp;
            DWORD64 Lr;
        };
        DWORD64 X[31];
    };
    DWORD64 Sp;
    DWORD64 Pc;
    ARM64_NT_NEON128 V[32];
    DWORD Fpcr;
    DWORD Fpsr;
    DWORD Bcr[8];
    DWORD64 Bvr[8];
    DWORD Wcr[2];
    DWORD64 Wvr[2];
};
using PARM64_NT_CONTEXT = ARM64_NT_CONTEXT*;

struct EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PARM64_NT_CONTEXT ContextRecord;
};

struct IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY {
    DWORD BeginAddress;
    union {
        DWORD UnwindData;
        struct {
            DWORD Flag : 2;
            DWORD FunctionLength : 11;
            DWORD RegF : 3;
            DWORD RegI : 4;
            DWORD H : 1;
            DWORD CR : 2;
            DWORD FrameSize : 9;
        };
    };
};

using PARM64_RUNTIME_FUNCTION = IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY*;

struct KNONVOLATILE_CONTEXT_POINTERS_ARM64 {
    PDWORD64 X19, X20, X21, X22, X23, X24, X25, X26, X27, X28;
    PDWORD64 Fp;
    PDWORD64 Lr;
    PDWORD64 D8, D9, D10, D11, D12, D13, D14, D15;
};

union DISPATCHER_CONTEXT_NONVOLREG_ARM64 {
    BYTE Buffer[11 * sizeof(DWORD64) + 8 * sizeof(double)];
    struct {
        DWORD64 GpNvRegs[11];
        double FpNvRegs[8];
    };
};

struct UNWIND_HISTORY_TABLE;
using EXCEPTION_ROUTINE = LONG(EXCEPTION_RECORD*, PVOID, ARM64_NT_CONTEXT*, PVOID);
using PEXCEPTION_ROUTINE = EXCEPTION_ROUTINE*;

struct DISPATCHER_CONTEXT_ARM64 {
    ULONG_PTR ControlPc;
    ULONG_PTR ImageBase;
    IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY* FunctionEntry;
    ULONG_PTR EstablisherFrame;
    ULONG_PTR TargetPc;
    PARM64_NT_CONTEXT ContextRecord;
    PEXCEPTION_ROUTINE LanguageHandler;
    PVOID HandlerData;
    UNWIND_HISTORY_TABLE* HistoryTable;
    DWORD ScopeIndex;
    BOOLEAN ControlPcIsUnwound;
    PBYTE NonVolatileRegisters;
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
