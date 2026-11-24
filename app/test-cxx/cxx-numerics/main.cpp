#include <chrono>
#include <complex>
#include <cstdio>
#include <limits>
#include <numeric>
#include <random>
#include <ratio>
#include <type_traits>

#define FAIL(what) do { std::fprintf(stderr, "cxx-numerics: %s failed\n", what); return 1; } while (0)

int main()
{
    if (std::numeric_limits<int>::max() <= 0 ||
        std::numeric_limits<unsigned int>::max() == 0)
        FAIL("numeric_limits");
    if (!std::numeric_limits<double>::is_iec559)
        FAIL("ieee floating point");

    if (std::gcd(12, 18) != 6 || std::lcm(4, 6) != 12)
        FAIL("gcd/lcm");

    using half = std::ratio<1, 2>;
    using doubled = std::ratio_multiply<half, std::ratio<2, 1>>;
    static_assert(std::ratio_equal_v<half, std::ratio<2, 4>>, "ratio_equal");
    static_assert(std::ratio_equal_v<doubled, std::ratio<1, 1>>, "ratio_multiply");

    const std::complex<double> value{3.0, 4.0};
    if (std::norm(value) != 25.0 || std::real(value) != 3.0 || std::imag(value) != 4.0)
        FAIL("complex");

    const auto start = std::chrono::steady_clock::now();
    const auto later = start + std::chrono::milliseconds(50);
    if (later - start != std::chrono::milliseconds(50))
        FAIL("chrono arithmetic");
    const std::chrono::milliseconds milliseconds(1250);
    if (std::chrono::duration_cast<std::chrono::seconds>(milliseconds).count() != 1)
        FAIL("chrono duration_cast");

    std::mt19937 generator{12345};
    std::uniform_int_distribution<int> distribution(1, 6);
    int minimum = 7;
    int maximum = 0;
    long long total = 0;
    for (int index = 0; index < 1000; ++index) {
        const int roll = distribution(generator);
        if (roll < minimum)
            minimum = roll;
        if (roll > maximum)
            maximum = roll;
        total += roll;
    }
    if (minimum < 1 || maximum > 6 || total <= 1000 || total >= 6000)
        FAIL("uniform_int_distribution");

    std::puts("cxx-numerics: numbers, chrono, and random ok");
    return 0;
}
