__declspec(dllimport) int cycle_a_value(void);
__declspec(dllexport) int cycle_b_value(void) { return cycle_a_value() + 2; }
