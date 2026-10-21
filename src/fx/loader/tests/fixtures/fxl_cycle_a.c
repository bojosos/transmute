__declspec(dllimport) int cycle_b_value(void);
__declspec(dllexport) int cycle_a_value(void) { return 1; }
__declspec(dllexport) int cycle_a_calls_b(void) { return cycle_b_value() * 10; }
