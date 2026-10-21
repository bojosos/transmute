// Forwarder target: a named export and an export by ordinal only (#3).
__declspec(dllexport) int target_answer(void) { return 42; }
int target_by_ordinal(void) { return 33; }
