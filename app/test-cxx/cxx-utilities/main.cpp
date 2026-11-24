#include <any>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#define FAIL(what) do { std::fprintf(stderr, "cxx-utilities: %s failed\n", what); return 1; } while (0)

int main()
{
    std::pair<int, std::string> pair{1, "one"};
    if (pair.first != 1 || pair.second != "one")
        FAIL("pair");

    const auto tuple = std::make_tuple(1, std::string("two"), 3.0);
    if (std::get<0>(tuple) != 1 || std::get<1>(tuple) != std::string("two"))
        FAIL("tuple");
    const auto [number, word, real] = tuple;
    if (number != 1 || word != "two" || real != 3.0)
        FAIL("structured bindings");

    std::optional<int> optional;
    if (optional.has_value() || optional.value_or(7) != 7)
        FAIL("optional empty");
    optional = 5;
    if (!optional.has_value() || *optional != 5)
        FAIL("optional set");

    std::variant<int, std::string> variant = std::string("x");
    if (!std::holds_alternative<std::string>(variant) ||
        std::get<std::string>(variant) != "x")
        FAIL("variant string");
    variant = 3;
    if (!std::holds_alternative<int>(variant) || std::get<int>(variant) != 3)
        FAIL("variant int");

    std::any any = 42;
    if (!any.has_value() || std::any_cast<int>(any) != 42)
        FAIL("any int");
    any = std::string("s");
    if (std::any_cast<std::string>(any) != "s")
        FAIL("any string");

    const auto unique = std::make_unique<int>(5);
    const auto shared = std::make_shared<int>(6);
    std::weak_ptr<int> weak = shared;
    if (*unique != 5 || *shared != 6)
        FAIL("unique/shared");
    if (const auto locked = weak.lock()) {
        if (*locked != 6)
            FAIL("weak lock value");
    } else {
        FAIL("weak lock");
    }
    if (shared.use_count() != 1)
        FAIL("weak use count");
    const auto shared2 = shared;
    if (shared.use_count() != 2)
        FAIL("shared use count");

    std::function<int(int)> twice = [](int value) { return value * 2; };
    if (twice(4) != 8)
        FAIL("function");

    int base = 10;
    const auto add = std::bind(std::plus<int>{}, std::ref(base), std::placeholders::_1);
    base = 20;
    if (add(5) != 25)
        FAIL("bind reference");
    std::reference_wrapper<int> reference = base;
    reference.get() = 30;
    if (base != 30)
        FAIL("reference_wrapper");
    if (std::invoke(add, 2) != 32)
        FAIL("invoke");

    int source = 1;
    const int previous = std::exchange(source, 5);
    if (previous != 1 || source != 5)
        FAIL("exchange");
    std::string origin = "move";
    const std::string target = std::move(origin);
    if (target != "move")
        FAIL("move");

    static_assert(std::is_same_v<std::decay_t<const int &>, int>, "decay");
    static_assert(std::is_integral_v<int> && !std::is_integral_v<double>, "integral");
    static_assert(std::is_same_v<std::enable_if_t<true, int>, int>, "enable_if");
    static_assert(std::is_same_v<std::make_integer_sequence<int, 3>,
                                 std::integer_sequence<int, 0, 1, 2>>, "integer_sequence");

    std::puts("cxx-utilities: pair, tuple, variants, and smart pointers ok");
    return 0;
}
