// DllMain(DLL_PROCESS_DETACH) waits for the process's worker threads, as
// thread pools (TBB, Blender's task scheduler) do when they are torn down.
// With Windows ExitProcess semantics the workers are already terminated, so
// the wait returns at once; detach code that ran while they were still alive
// would wait forever.
__declspec(dllimport) unsigned long FxlWaitForWorkers(void* reserved);

__declspec(dllexport) volatile int exitwait_attached;

int DllMain(void* module, unsigned long reason, void* reserved) {
    (void)module;
    if (reason == 1) exitwait_attached = 1;
    if (reason == 0) FxlWaitForWorkers(reserved);
    return 1;
}
