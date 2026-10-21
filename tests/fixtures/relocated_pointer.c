static volatile int value = 42;
static volatile int* volatile pointer_to_value = &value;

int guest_entry(void) {
    return *pointer_to_value;
}
