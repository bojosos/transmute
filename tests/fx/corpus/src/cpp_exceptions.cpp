// C++ exceptions: throw/catch across functions, destructors during unwinding,
// hierarchies, rethrow, exception_ptr, constructors that throw, RTTI.
#include <stdio.h>

#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

static std::string trace;

struct Noisy {
    std::string name;
    explicit Noisy(std::string n) : name(std::move(n)) { trace += "+" + name; }
    ~Noisy() { trace += "-" + name; }
};

struct Base : std::exception {
    int code;
    explicit Base(int c) : code(c) {}
    const char* what() const noexcept override { return "Base"; }
};
struct Derived : Base {
    std::string detail;
    Derived(int c, std::string d) : Base(c), detail(std::move(d)) {}
    const char* what() const noexcept override { return "Derived"; }
};
struct Unrelated {
    int value;
};

__declspec(noinline) static void level3(int kind) {
    Noisy n("L3");
    switch (kind) {
    case 0: throw 42;
    case 1: throw std::runtime_error("runtime failure");
    case 2: throw Derived(7, "detail-text");
    case 3: throw Unrelated{99};
    case 4: throw std::string("string payload");
    case 5: throw 2.5;
    case 6: throw "c-string";
    default: break;
    }
}

__declspec(noinline) static void level2(int kind) {
    Noisy n("L2");
    std::vector<Noisy> many;
    many.reserve(3);
    many.emplace_back("V1");
    many.emplace_back("V2");
    level3(kind);
}

__declspec(noinline) static void level1(int kind) {
    Noisy n("L1");
    try {
        level2(kind);
    } catch (const std::out_of_range&) {
        trace += "[wrong]";
    }
}

static void test_kinds() {
    for (int kind = 0; kind <= 7; kind++) {
        trace.clear();
        std::string caught;
        try {
            level1(kind);
            caught = "none";
        } catch (int v) {
            caught = "int " + std::to_string(v);
        } catch (const Derived& d) {
            caught = std::string("Derived code=") + std::to_string(d.code) + " " + d.detail + " what=" + d.what();
        } catch (const std::exception& e) {
            caught = std::string("std::exception ") + e.what();
        } catch (const std::string& s) {
            caught = "string " + s;
        } catch (double d) {
            caught = "double " + std::to_string(d);
        } catch (const char* s) {
            caught = std::string("const char* ") + s;
        } catch (...) {
            caught = "ellipsis";
        }
        printf("kind %d: %s trace=%s\n", kind, caught.c_str(), trace.c_str());
    }
}

static void test_base_catch() {
    try {
        throw Derived(3, "via-base");
    } catch (Base& b) {
        printf("caught by base: code=%d what=%s dynamic_cast=%d typeid=%s\n", b.code, b.what(),
               dynamic_cast<Derived*>(&b) != nullptr, typeid(b).name());
    }
}

static void test_rethrow() {
    trace.clear();
    try {
        try {
            Noisy n("R");
            throw std::logic_error("first");
        } catch (std::exception& e) {
            trace += "[inner ";
            trace += e.what();
            trace += "]";
            throw;
        }
    } catch (const std::logic_error& e) {
        trace += "[outer ";
        trace += e.what();
        trace += "]";
    }
    try {
        try {
            throw 1;
        } catch (int) {
            throw std::runtime_error("replaced");
        }
    } catch (const std::exception& e) {
        trace += "[replaced ";
        trace += e.what();
        trace += "]";
    }
    printf("rethrow trace=%s\n", trace.c_str());
}

static void test_exception_ptr() {
    std::exception_ptr saved;
    try {
        throw std::invalid_argument("stored");
    } catch (...) {
        saved = std::current_exception();
    }
    printf("exception_ptr null=%d uncaught=%d\n", saved == nullptr, std::uncaught_exceptions());
    try {
        std::rethrow_exception(saved);
    } catch (const std::invalid_argument& e) {
        printf("rethrow_exception: %s\n", e.what());
    }
    std::exception_ptr made = std::make_exception_ptr(Derived(11, "made"));
    try {
        std::rethrow_exception(made);
    } catch (const Base& b) {
        printf("make_exception_ptr code=%d\n", b.code);
    }
    try {
        try {
            throw std::runtime_error("inner-nested");
        } catch (...) {
            std::throw_with_nested(std::logic_error("outer-nested"));
        }
    } catch (const std::logic_error& e) {
        printf("nested outer=%s", e.what());
        try {
            std::rethrow_if_nested(e);
        } catch (const std::runtime_error& inner) {
            printf(" inner=%s", inner.what());
        }
        printf("\n");
    }
}

struct Member {
    int id;
    explicit Member(int i) : id(i) { trace += "+M" + std::to_string(id); }
    ~Member() { trace += "-M" + std::to_string(id); }
};

struct Composite {
    Member a, b, c;
    explicit Composite(bool fail) : a(1), b(2), c((fail ? throw std::runtime_error("ctor") : 0, 3)) {}
};

static void test_constructor_throw() {
    trace.clear();
    try {
        Composite ok(false);
        Composite bad(true);
    } catch (const std::exception& e) {
        trace += std::string("[") + e.what() + "]";
    }
    printf("constructor trace=%s\n", trace.c_str());

    trace.clear();
    try {
        auto p = std::make_unique<Noisy>("U");
        auto s = std::make_shared<Noisy>("S");
        throw 5;
    } catch (int) {
        trace += "[caught]";
    }
    printf("smart pointer trace=%s\n", trace.c_str());
}

static void test_std_exceptions() {
    std::vector<int> v(3);
    try {
        v.at(10) = 1;
    } catch (const std::out_of_range&) {
        printf("vector::at threw out_of_range\n");
    }
    try {
        std::string s("abc");
        (void)s.substr(10);
    } catch (const std::out_of_range&) {
        printf("string::substr threw out_of_range\n");
    }
    try {
        size_t huge = static_cast<size_t>(1) << 62;
        std::vector<char> big;
        big.reserve(huge);
    } catch (const std::length_error&) {
        printf("reserve threw length_error\n");
    } catch (const std::bad_alloc&) {
        printf("reserve threw bad_alloc\n");
    }
    try {
        void* p = ::operator new(static_cast<size_t>(1) << 62);
        printf("operator new returned %d\n", p != nullptr);
    } catch (const std::bad_alloc& e) {
        printf("operator new threw bad_alloc what=%s\n", e.what());
    }
    static int* volatile nothrow_p;
    volatile size_t huge_count = static_cast<size_t>(1) << 59;
    nothrow_p = new (std::nothrow) int[huge_count];
    printf("nothrow new null=%d\n", nothrow_p == nullptr);
    delete[] nothrow_p;
    try {
        (void)std::stoi("not-a-number");
    } catch (const std::invalid_argument&) {
        printf("stoi threw invalid_argument\n");
    }
    try {
        Base& b = *new Base(1);
        Derived& d = dynamic_cast<Derived&>(b);
        (void)d;
    } catch (const std::bad_cast& e) {
        printf("dynamic_cast threw bad_cast\n");
    }
}

static int deep(int n) {
    Noisy guard(n % 10 == 0 ? "D" + std::to_string(n) : std::string());
    if (n == 0) throw n;
    return deep(n - 1) + 1;
}

static void test_deep_and_loop() {
    trace.clear();
    try {
        deep(40);
    } catch (int v) {
        printf("deep caught=%d", v);
    }
    size_t plus = 0, minus = 0;
    for (char ch : trace) {
        plus += ch == '+';
        minus += ch == '-';
    }
    printf(" constructed=%zu destroyed=%zu\n", plus, minus);
    long long total = 0;
    for (int i = 0; i < 2000; i++) {
        try {
            if (i % 2) throw i;
            total += 1;
        } catch (int v) {
            total += v;
        }
    }
    printf("loop total=%lld\n", total);
}

static void noexcept_helper() noexcept {
    try {
        throw 1;
    } catch (int) {
    }
}

int main() {
    test_kinds();
    test_base_catch();
    test_rethrow();
    test_exception_ptr();
    test_constructor_throw();
    test_std_exceptions();
    test_deep_and_loop();
    noexcept_helper();
    printf("uncaught at end=%d\n", std::uncaught_exceptions());
    fflush(stdout);
    return 0;
}
