// C++ hello for the toolchain checks: virtual calls, new/delete and an exception.
// No SDK or libc++ headers: the few declarations needed are written here.
extern "C" int printf(const char *format, ...);

struct Shape {
    virtual ~Shape() = default;
    virtual int sides() const = 0;
};
struct Square : Shape { int sides() const override { return 4; } };

static int checked(int value) {
    if (value < 0) throw value;
    return value;
}

int main() {
    Shape *shape = new Square;
    int sides = shape->sides();
    delete shape;
    int caught = 0;
    try { checked(-7); } catch (int value) { caught = value; }
    printf("hello from clang++: sides=%d caught=%d\n", sides, caught);
    return sides == 4 && caught == -7 ? 0 : 1;
}
