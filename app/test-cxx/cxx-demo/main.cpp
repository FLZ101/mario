#include <algorithm>
#include <charconv>
#include <cstdio>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <variant>
#include <vector>

int main() {
    std::string name = "ma";
    name += "rio";
    if (name != "mario") {
        std::fputs("cxx-demo: std::string failed\n", stderr);
        return 1;
    }

    std::vector<int> values{3, 1, 4};
    values.push_back(2);
    std::sort(values.begin(), values.end());
    if (values != std::vector<int>{1, 2, 3, 4}) {
        std::fputs("cxx-demo: vector or sort failed\n", stderr);
        return 1;
    }

    if (std::accumulate(values.begin(), values.end(), 0) != 10) {
        std::fputs("cxx-demo: std::accumulate failed\n", stderr);
        return 1;
    }

    std::unordered_map<std::string, int> counts{{name, 4}};
    const auto [existing, inserted] = counts.emplace(name, 10);
    if (inserted || existing == counts.end() || existing->second != 4) {
        std::fputs("cxx-demo: std::unordered_map duplicate failed\n", stderr);
        return 1;
    }
    const auto erased = counts.erase(name);
    if (erased != 1 || counts.find(name) != counts.end()) {
        std::fputs("cxx-demo: std::unordered_map erase failed\n", stderr);
        return 1;
    }

    std::optional<std::string> label;
    if (label.has_value() || label.value_or("empty") != "empty") {
        std::fputs("cxx-demo: std::optional fallback failed\n", stderr);
        return 1;
    }
    label.emplace(name);
    if (!label.has_value() || label.value_or("empty") != name) {
        std::fputs("cxx-demo: std::optional stored value failed\n", stderr);
        return 1;
    }

    std::variant<int, std::string> entry = name;
    if (!std::holds_alternative<std::string>(entry) ||
        std::get<std::string>(entry) != name) {
        std::fputs("cxx-demo: std::variant string lookup failed\n", stderr);
        return 1;
    }
    entry = 42;
    if (!std::holds_alternative<int>(entry) || std::get<int>(entry) != 42) {
        std::fputs("cxx-demo: std::variant type change failed\n", stderr);
        return 1;
    }

    char buffer[16];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), -42);
    if (written.ec != std::errc{} ||
        std::string_view(buffer, written.ptr - buffer) != "-42") {
        std::fputs("cxx-demo: std::to_chars failed\n", stderr);
        return 1;
    }

    int parsed = 0;
    const auto valid = std::from_chars(buffer, written.ptr, parsed);
    if (valid.ec != std::errc{} || valid.ptr != written.ptr || parsed != -42) {
        std::fputs("cxx-demo: std::from_chars conversion failed\n", stderr);
        return 1;
    }
    const auto invalid =
        std::from_chars(name.data(), name.data() + name.size(), parsed);
    if (invalid.ec != std::errc::invalid_argument ||
        invalid.ptr != name.data() || parsed != -42) {
        std::fputs("cxx-demo: std::from_chars invalid input failed\n", stderr);
        return 1;
    }

    std::puts("cxx-demo: standard library ok");
    return 0;
}
