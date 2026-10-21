// Startup and shutdown order: C initializers (.CRT$XIU), C++ static
// constructors, thread-safe function-local statics under contention,
// atexit handlers and static destructors after main returns.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

static std::string order;

static int __cdecl c_initializer() {
    order += "C;";
    return 0;
}
#pragma section(".CRT$XIU", long, read)
__declspec(allocate(".CRT$XIU")) __attribute__((used)) static int(__cdecl* const c_init_entry)() = c_initializer;

struct Global {
    const char* name;
    explicit Global(const char* n) : name(n) { order += std::string("ctor ") + name + ";"; }
    ~Global() { printf("dtor %s\n", name); }
};

static Global first("first");
static Global second("second");
static int dynamic_value = static_cast<int>(order.size()) * 2;
inline Global inline_global("inline");

template <typename T>
struct Holder {
    static Global instance;
};
template <typename T>
Global Holder<T>::instance("template");
template struct Holder<int>;

static std::atomic<int> local_constructions{0};
struct Expensive {
    int value;
    Expensive() : value(42) {
        local_constructions++;
        Sleep(20);
    }
};

static Expensive& get_expensive() {
    static Expensive e;
    return e;
}

static void at_exit_one() {
    printf("atexit one\n");
}
static void at_exit_two() {
    printf("atexit two\n");
    fflush(stdout);
}

int main() {
    printf("order=%s\n", order.c_str());
    printf("dynamic_value=%d\n", dynamic_value);
    std::vector<std::thread> threads;
    std::atomic<int> sum{0};
    for (int i = 0; i < 8; i++) threads.emplace_back([&] { sum += get_expensive().value; });
    for (auto& t : threads) t.join();
    printf("local static constructions=%d sum=%d\n", local_constructions.load(), sum.load());
    atexit(at_exit_one);
    atexit(at_exit_two);
    static Global late("function-static");
    printf("main returns\n");
    return 3;
}
