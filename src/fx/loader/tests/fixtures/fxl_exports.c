// Exports by name, by ordinal only, data, forwarders (guest, host, by
// ordinal, chained) and a writable+executable section.
int named_one(void) { return 1; }
int by_ordinal(void) { return 5; }
int data_value = 77;

#pragma section(".rwx", read, write, execute)
__declspec(allocate(".rwx")) volatile int rwx_data = 5;

int rwx_probe(void) { return rwx_data; }
