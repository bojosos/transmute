# fx abi: signature grammar and thunk protocol

## Signature grammar (sigdb)

```
signature := '?'                         unknown (see below)
           | ret '(' { param } [ '.' ] ')'   '.' = variadic, always last
ret       := 'v' | type
param     := type
type      := 'c' | 'C'      int8 / uint8 (char, signed char / unsigned char, BYTE, BOOLEAN, bool)
           | 's' | 'S'      int16 / uint16 (short / WORD, wchar_t, WCHAR)
           | 'i' | 'I'      int32 / uint32 (int, long, BOOL, HRESULT / UINT, DWORD, ULONG)
           | 'l'            64-bit integer (long long, SIZE_T, LPARAM, INT_PTR)
           | 'p'            data pointer or handle (also unprototyped function pointers)
           | 'f' | 'd'      float / double (long double is double on both targets)
           | '{' N [':16'] '}'   integer-class aggregate of N bytes (":16" = 16-byte aligned)
           | '{f' K '}' | '{d' K '}'   homogeneous float/double aggregate of K (1..4) members
           | '@' signature  function pointer with a known prototype
```

Examples: `CreateFileW` = `p(pIIpIIp)`, `qsort` = `v(pll@i(pp))`,
`signal` = `@v(i)(i@v(i))`, `wsprintfA` = `i(pp.)`, `conj` = `{d2}({d2})`,
`MonitorFromPoint` = `p({8}I)`.

`?` (unknown, also the default for functions missing from the database):
x0-x7 go to the first eight Win64 positions, eight further guest stack slots
are forwarded, v0-v3 are copied to xmm0-xmm3, and the result is returned
both ways (rax -> x0 and xmm0 -> v0). For `host_callable(fn, nullptr)` the
mirror image applies.

The table is `generated/sigdb_table.inc`, produced by `tools/sigdb/sigdb.py`
from clang's JSON AST of the Windows SDK and UCRT headers (target
x86_64-pc-windows-msvc, C mode). Record sizes and alignments come from a
second clang pass (`sizeof`/`_Alignof`); HFA detection follows AAPCS64 over
the record's fields. `sigdb.cpp` holds a small built-in table for C++-only
declarations (`set_terminate`, `_set_new_handler`, ...) consulted first.

## Calling-convention mapping

ARM64 side (Windows AAPCS64): integers in x0-x7, FP in v0-v7, HFAs in
consecutive v registers (or entirely on the stack), aggregates up to 16 bytes
in one or two x registers (even pair when 16-byte aligned), larger aggregates
by reference, stack slots of 8 bytes. Variadic functions put *every*
argument (named ones too) on the imaginary stack whose first 64 bytes are
x0-x7; no FP registers. Results: x0 (x0/x1 for aggregates <= 16 bytes),
v0 (v0-v3 for HFAs), x8 = result buffer for larger aggregates.

Win64 side: positions 0-3 in rcx/rdx/r8/r9 or xmm0-3, 32-byte home area,
further positions at [rsp+8*pos]; aggregates of 1/2/4/8 bytes by value as
integers (HFAs included), all others by reference to a 16-byte aligned copy;
non-1/2/4/8 aggregate results through a hidden pointer in rcx. Variadic
calls duplicate the first four GPR positions into xmm0-3. Variadic thunks
forward the remaining x registers plus 16 guest stack slots (never reading at
or above `ctx->guest_stack_base`).

## Thunk protocol

A guest->host thunk is registered with `register_host_entry(thunk, thunk)`
and entered like a translated region: state flushed, r15 = ctx + 128, rsp
16-byte aligned with 32 bytes of callee shadow space. It

1. keeps x30 in rbx (and the guest SP in rsi) for the whole call, so
   callbacks that reuse ctx cannot disturb the return; signatures with at
   most four positions and no temporaries allocate no frame (the entry
   shadow space is enough), others `sub rsp, F`;
2. converts function-pointer arguments through `wrap_callback_argument`
   (guest code -> `host_callable` trampoline, plain abi thunk -> its native
   target, host pointers and sentinels such as SIG_IGN unchanged);
3. builds the Win64 frame, calls the native function, stores the result
   (narrow integers are sign/zero-extended; function-pointer results go
   through `unwrap_callback_result`);
4. copies TEB.LastErrorValue (gs:[0x68]) to `ctx->shadow_teb + 0x68`;
5. writes x30 back to `ctx->x[30]` (nested callbacks clobber it and the
   runtime's scrubbed RSB entries, `rsb_redispatch`, re-read it), then
   returns: if `rsb[(rsb_top - 1) & 31].guest_return == x30` it pops the
   entry (`rsb_top = (rsb_top - 1) & 31`) and jumps to its `host_entry`;
   otherwise `ctx->pc = x30; jmp [r15 + stub_indirect]`.

`abi_code_frame()` (process.hpp) describes each piece of abi code for the
runtime's arena unwinder: thunks are entered by jmp at rsp == host_rsp and
have `frame_bytes` allocated between `prolog_bytes` and `epilog_offset`;
callback trampolines are ordinary Win64 functions (`entered_by_call`).

`bind_unknown_host_target()` on one of our trampolines returns a thunk with
the trampoline's signature (the runtime treats a non-entry result as host
code); `unwrap_callback_result()` hands the guest its own function instead.

Override thunks set `ctx->pc = x30`, call `handler(ctx, binding)` and then
either return as above or (`OverrideResult::resume`) jump to
`[r15 + stub_indirect]` with the handler's `ctx->pc`.

Window procedures live in structures and window/class data, out of reach
of the generic wrapping, so `RegisterClass*`, `{Set,Get}{Window,Class}LongPtr*`
(GWLP_WNDPROC/GCLP_WNDPROC) and `GetClassInfo*` are overridden to install
`l(pIll)` trampolines and to hand back guest procedures.

Callback trampolines are ordinary Win64 functions: they home rcx/rdx/r8/r9,
save xmm0-3 and call `callback_dispatch`, which maps the Win64 arguments into
the calling thread's context (`current_context()` or
`ensure_thread_context()`), pushes stack arguments below the guest SP, runs
`call_guest_prepared` and converts the result back.

Some host APIs never run callbacks that live outside a loaded module (the
code arena is private memory): the legacy pool (`QueueUserWorkItem`,
`RtlQueueWorkItem`) reports success and drops them, and `QueueUserAPC`
routines in the arena never ran under transmute-fx. Those overrides queue a
function of the fx image that then calls the trampoline.

## Fibers

On ARM64 `GetCurrentFiber()`/`GetFiberData()` are inline reads of the guest
TEB (`x18 + 0x20`, then `*(void**)fiber`). A guest fiber is a `GuestFiber`
(first field = fiber parameter) backed by a host fiber (host stacks, host FLS
and the FLS callbacks of `DeleteFiber` stay per fiber) and a runtime guest
stack (`allocate_guest_stack`). `SwitchToFiber` saves the per-fiber part of
GuestContext (registers, pc, host_rsp, nesting, RSB, spills), makes the
target's stack current with `switch_guest_stack`, sets the shadow TEB
`FiberData`/`GuaranteedStackBytes`, and switches host fibers; the resumed
side restores its own snapshot. A new fiber starts in `fiber_host_start`,
which runs the guest start routine with `call_guest_prepared` (returning from
it ends the thread). FPCR switches only with `FIBER_FLAG_FLOAT_SWITCH`.

## Process exit

`exit`, `_exit`, `quick_exit`, `abort`, `ExitProcess`, `RtlExitUserProcess`
and `TerminateProcess`/`NtTerminateProcess` on our own process end in
`loader_process_exit`. `exit` first runs the guest's
`_register_thread_local_exe_atexit_callback` callback (DLL_PROCESS_DETACH)
and atexit table; the host UCRT accepts only one such registration per
process (transmute-fx.exe owns it) and fail-fasts on a second, so the guest's
is kept by the ABI.

## Diagnostics

`TRANSMUTE_FX_TRACE_IMPORTS=1` (or `first`) logs every call through a host
thunk or override, `fx call <dll>!<name> from <module>+0x<off>`, and every
callback entry, `fx callback <function> <signature>`. Thunks bound while it is
off contain no tracing code. `TRANSMUTE_FX_TRACE_EXIT=1` logs the exit paths
above with their guest caller. Both write straight to the stderr handle and
preserve the thread's last error.
