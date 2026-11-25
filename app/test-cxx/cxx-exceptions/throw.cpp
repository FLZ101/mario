#include <stdexcept>

// Cross-TU throw target for app/test-cxx/cxx-exceptions/main.cpp.
void throw_runtime_error()
{
    throw std::runtime_error("cxx-exceptions: cross-tu throw caught");
}
