#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>

#define FAIL(what) do { std::fprintf(stderr, "cxx-strings: %s failed\n", what); return 1; } while (0)

int main()
{
    std::string text = "hello";
    text += ", world";
    if (text.size() != 12 || text.compare("hello, world") != 0)
        FAIL("append/compare");

    text.insert(5, " there");
    if (text != "hello there, world")
        FAIL("insert");
    text.erase(5, 6);
    if (text != "hello, world")
        FAIL("erase");
    text.replace(7, 5, "mario");
    if (text != "hello, mario")
        FAIL("replace");

    if (text.find('o') != 4 || text.rfind('o') != 11 || text.find("mario") != 7)
        FAIL("find/rfind");
    if (text.find_first_of("xyz") != std::string::npos)
        FAIL("find_first_of");
    if (text.find_first_not_of("hello, ") != 7)
        FAIL("find_first_not_of");
    if (text.substr(0, 5) != "hello" || text.substr(7) != "mario")
        FAIL("substr");

    std::string_view view = text;
    if (view.substr(0, 5) != "hello" || view.back() != 'o' || view.find("mario") != 7)
        FAIL("string_view");
    const std::string_view empty;
    if (!empty.empty())
        FAIL("string_view empty");

    if (std::to_string(42) != "42" || std::to_string(2.5) != "2.500000")
        FAIL("to_string");
    if (std::stoi("42") != 42 || std::stod("2.5") != 2.5)
        FAIL("stoi/stod");

    char buffer[32];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), 1234);
    if (written.ec != std::errc{} ||
        std::string_view(buffer, written.ptr - buffer) != "1234")
        FAIL("to_chars");
    int parsed = 0;
    const auto parsed_result = std::from_chars(buffer, written.ptr, parsed);
    if (parsed_result.ec != std::errc{} || parsed_result.ptr != written.ptr || parsed != 1234)
        FAIL("from_chars");
    const char invalid[] = "abc";
    const auto invalid_result = std::from_chars(invalid, invalid + 3, parsed);
    if (invalid_result.ec != std::errc::invalid_argument || invalid_result.ptr != invalid)
        FAIL("from_chars invalid");

    std::puts("cxx-strings: string and character conversion ok");
    return 0;
}
