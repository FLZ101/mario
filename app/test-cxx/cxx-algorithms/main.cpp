#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <deque>
#include <forward_list>
#include <iterator>
#include <list>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define FAIL(what) do { std::fprintf(stderr, "cxx-algorithms: %s failed\n", what); return 1; } while (0)

int main()
{
    std::vector<int> values{5, 1, 4, 2, 3, 3};
    std::sort(values.begin(), values.end());
    if (values != std::vector<int>({1, 2, 3, 3, 4, 5}))
        FAIL("sort");

    std::stable_sort(values.begin(), values.end(), std::greater<int>());
    if (values != std::vector<int>({5, 4, 3, 3, 2, 1}))
        FAIL("stable_sort");
    std::sort(values.begin(), values.end());

    std::vector<int> doubled;
    std::transform(values.begin(), values.end(), std::back_inserter(doubled),
                   [](int value) { return value * 2; });
    if (doubled != std::vector<int>({2, 4, 6, 6, 8, 10}))
        FAIL("transform");
    if (std::accumulate(values.begin(), values.end(), 0) != 18)
        FAIL("accumulate");

    int partial[6] = {};
    int adjacent[6] = {};
    std::partial_sum(values.begin(), values.end(), partial);
    std::adjacent_difference(values.begin(), values.end(), adjacent);
    if (partial[5] != 18 || adjacent[1] != 1)
        FAIL("partial_sum/adjacent_difference");

    std::reverse(doubled.begin(), doubled.end());
    if (doubled != std::vector<int>({10, 8, 6, 6, 4, 2}))
        FAIL("reverse");

    std::vector<int> rotated{1, 2, 3, 4, 5};
    std::rotate(rotated.begin(), rotated.begin() + 2, rotated.end());
    if (rotated != std::vector<int>({3, 4, 5, 1, 2}))
        FAIL("rotate");

    std::vector<int> deduped{1, 1, 2, 2, 2, 3};
    const auto unique_end = std::unique(deduped.begin(), deduped.end());
    if (std::distance(deduped.begin(), unique_end) != 3 || deduped[0] != 1 ||
        deduped[1] != 2 || deduped[2] != 3)
        FAIL("unique");

    std::vector<int> partitioned{1, 2, 3, 4, 5, 6};
    const auto partition_point = std::partition(partitioned.begin(), partitioned.end(),
                                                [](int value) { return value % 2 == 0; });
    if (!std::is_partitioned(partitioned.begin(), partitioned.end(),
                             [](int value) { return value % 2 == 0; }) ||
        !std::all_of(partitioned.begin(), partition_point,
                     [](int value) { return value % 2 == 0; }) ||
        !std::none_of(partition_point, partitioned.end(),
                      [](int value) { return value % 2 == 0; }))
        FAIL("partition");

    std::vector<int> nth{5, 3, 1, 4, 2};
    std::nth_element(nth.begin(), nth.begin() + 2, nth.end());
    if (nth[2] != 3)
        FAIL("nth_element");

    if (!std::binary_search(values.begin(), values.end(), 3) ||
        std::binary_search(values.begin(), values.end(), 9) ||
        *std::lower_bound(values.begin(), values.end(), 3) != 3 ||
        *std::upper_bound(values.begin(), values.end(), 3) != 4)
        FAIL("binary_search/lower_bound/upper_bound");
    const auto range = std::equal_range(values.begin(), values.end(), 3);
    if (range.first == values.end() || *range.first != 3 ||
        std::distance(range.first, range.second) != 2)
        FAIL("equal_range");

    const auto minmax = std::minmax_element(values.begin(), values.end());
    if (*minmax.first != 1 || *minmax.second != 5)
        FAIL("minmax_element");
    if (!std::all_of(values.begin(), values.end(), [](int value) { return value > 0; }) ||
        !std::any_of(values.begin(), values.end(), [](int value) { return value % 2 == 0; }) ||
        !std::none_of(values.begin(), values.end(), [](int value) { return value < 0; }))
        FAIL("all_of/any_of/none_of");
    if (static_cast<std::size_t>(std::count(values.begin(), values.end(), 3)) != 2 ||
        std::find(values.begin(), values.end(), 4) == values.end())
        FAIL("count/find");

    std::vector<int> heap{3, 1, 4, 1, 5};
    std::make_heap(heap.begin(), heap.end());
    if (heap.front() != 5)
        FAIL("make_heap");
    heap.push_back(9);
    std::push_heap(heap.begin(), heap.end());
    if (heap.front() != 9)
        FAIL("push_heap");
    std::pop_heap(heap.begin(), heap.end());
    if (heap.back() != 9)
        FAIL("pop_heap");
    heap.pop_back();
    std::sort_heap(heap.begin(), heap.end());
    if (!std::is_sorted(heap.begin(), heap.end()))
        FAIL("sort_heap");

    std::vector<int> permutation{1, 2, 3};
    if (!std::next_permutation(permutation.begin(), permutation.end()) ||
        permutation != std::vector<int>({1, 3, 2}))
        FAIL("next_permutation");

    std::set<int> ordered{3, 1, 2};
    if (ordered.size() != 3 || *ordered.begin() != 1 || *ordered.rbegin() != 3)
        FAIL("set");
    std::map<std::string, int> table{{"b", 2}, {"a", 1}};
    table["c"] = 3;
    if (table.size() != 3 || table.at("b") != 2)
        FAIL("map");

    std::unordered_set<int> hash_set{1, 2, 3};
    std::unordered_map<std::string, int> hash_map{{"a", 1}, {"b", 2}};
    hash_map["c"] = 3;
    if (hash_set.size() != 3 || hash_map.size() != 3 || hash_map.at("c") != 3)
        FAIL("unordered_set/unordered_map");

    std::deque<int> deque{1, 2, 3};
    deque.push_front(0);
    deque.push_back(4);
    if (deque.size() != 5 || deque.front() != 0 || deque.back() != 4)
        FAIL("deque");
    std::list<int> list{1, 2, 3};
    list.push_front(0);
    if (list.front() != 0)
        FAIL("list");
    std::forward_list<int> forward{1, 2, 3};
    forward.push_front(0);
    if (forward.front() != 0)
        FAIL("forward_list");

    std::array<int, 3> array{1, 2, 3};
    if (array[2] != 3)
        FAIL("array");
    std::bitset<8> bits("10101010");
    if (bits.count() != 4 || !bits.test(7) || bits.to_ulong() != 170)
        FAIL("bitset");

    std::puts("cxx-algorithms: containers and algorithms ok");
    return 0;
}
