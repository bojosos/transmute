__declspec(dllimport) int guest_answer(void);
__declspec(dllimport) int guest_bias;

int guest_entry(void) {
    volatile int answer = guest_answer();
    return answer + guest_bias;
}
