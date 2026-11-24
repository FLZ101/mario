#include <algorithm>
#include <cstdio>
#include <numeric>
#include <string>
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

    std::puts("cxx-demo: standard library ok");
    return 0;
}
