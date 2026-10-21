// Compiled as ARM64 PE32+ without a CRT to exercise the real Windows toolchain.
int guest_entry(void) {
    volatile int answer = 42;
    return answer;
}
