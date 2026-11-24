#include <stdexcept>

void throw_from_other_file(unsigned &order)
{
    struct Cleanup {
        unsigned &order;
        ~Cleanup() { order = order * 10 + 1; }
    } cleanup{order};

    throw std::runtime_error("mario exception");
}
