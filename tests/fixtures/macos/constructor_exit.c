static void raw_exit(int code) {
    register long arg __asm__("x0") = code;
    register long call __asm__("x16") = 1;
    __asm__ volatile("svc #0x80" : "+r"(arg) : "r"(call) : "memory", "cc");
}
__attribute__((constructor)) static void initialize(void) { raw_exit(9); }
int main(void) { return 99; }
