#include <cstdio>
#include <stdexcept>

void throw_runtime_error();

int main()
{
    // Cross-TU throw: exercises unwinding through a separate translation unit.
    try {
        throw_runtime_error();
    } catch (const std::runtime_error &error) {
        std::puts(error.what());
    }

    // Same-TU throw: verifies ordinary libc++ exception dispatch still works.
    try {
        throw std::out_of_range("cxx-exceptions");
    } catch (const std::out_of_range &) {
        std::puts("cxx-exceptions: exception handling ok");
        return 0;
    }

    return 1;
}
