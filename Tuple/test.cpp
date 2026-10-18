#include "Tuple.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

using lam::Tuple;

namespace {

[[noreturn]] void fail(std::string_view check, std::size_t iteration) {
    std::cerr << "FAIL: " << check << " (iteration=" << iteration << ")\n";
    std::abort();
}

template<typename Custom, typename Standard>
void check_values(const Custom& custom,
                  const Standard& standard,
                  std::size_t iteration) {
    if (custom.template get<0>() != std::get<0>(standard)) {
        fail("get<0>", iteration);
    }
    if (custom.template get<1>() != std::get<1>(standard)) {
        fail("get<1>", iteration);
    }
    if (custom.template get<2>() != std::get<2>(standard)) {
        fail("get<2>", iteration);
    }
}

template<typename Custom, typename Standard>
void check_relations(const Custom& left,
                     const Custom& right,
                     const Standard& standard_left,
                     const Standard& standard_right,
                     std::size_t iteration) {
    if ((left < right) != (standard_left < standard_right)) {
        fail("operator<", iteration);
    }
    if ((left > right) != (standard_left > standard_right)) {
        fail("operator>", iteration);
    }
    if ((left <= right) != (standard_left <= standard_right)) {
        fail("operator<=", iteration);
    }
    if ((left >= right) != (standard_left >= standard_right)) {
        fail("operator>=", iteration);
    }
}

void default_and_special_members() {
    Tuple<int, int, int> original{10, 20, 30};
    Tuple<int, int, int> copy_constructed{original};
    check_values(copy_constructed, std::tuple{10, 20, 30}, 0);

    Tuple<int, int, int> copy_assigned;
    copy_assigned = original;
    check_values(copy_assigned, std::tuple{10, 20, 30}, 0);

    Tuple<int, int, int> move_constructed{std::move(copy_constructed)};
    check_values(move_constructed, std::tuple{10, 20, 30}, 0);

    Tuple<int, int, int> move_assigned;
    move_assigned = std::move(copy_assigned);
    check_values(move_assigned, std::tuple{10, 20, 30}, 0);

    Tuple<int, int, int> defaults;
    check_values(defaults, std::tuple{0, 0, 0}, 0);

    auto made = Tuple<int, int, int>::make_tuple(7, 8, 9);
    check_values(made, std::tuple{7, 8, 9}, 0);
}

void integer_stress() {
    constexpr std::size_t iterations = 250'000;
    constexpr std::uint64_t seed = 0x7A91'E202'6ULL;

    std::mt19937_64 random(seed);
    std::uniform_int_distribution<int> values(-1'000'000, 1'000'000);

    for (std::size_t i = 0; i < iterations; ++i) {
        const int a0 = values(random);
        const int a1 = values(random);
        const int a2 = values(random);
        const int b0 = values(random);
        const int b1 = values(random);
        const int b2 = values(random);

        Tuple<int, int, int> left{a0, int{a1}, int{a2}};
        Tuple<int, int, int> right{b0, int{b1}, int{b2}};
        const auto standard_left = std::tuple{a0, a1, a2};
        const auto standard_right = std::tuple{b0, b1, b2};

        check_values(left, standard_left, i);
        check_values(right, standard_right, i);
        check_relations(left, right, standard_left, standard_right, i);
    }
}

void heterogeneous_stress() {
    constexpr std::size_t iterations = 50'000;
    constexpr std::uint64_t seed = 0x5171'6E6'2026ULL;

    std::mt19937_64 random(seed);
    std::uniform_int_distribution<int> values(-100'000, 100'000);

    for (std::size_t i = 0; i < iterations; ++i) {
        const int left_number = values(random);
        const int right_number = values(random);
        const std::string left_text = std::to_string(values(random));
        const std::string right_text = std::to_string(values(random));
        const long long left_tail = values(random);
        const long long right_tail = values(random);

        Tuple<int, std::string, long long> left{
            left_number, std::string{left_text}, static_cast<long long>(left_tail)};
        Tuple<int, std::string, long long> right{
            right_number, std::string{right_text}, static_cast<long long>(right_tail)};

        const auto standard_left =
            std::tuple{left_number, left_text, left_tail};
        const auto standard_right =
            std::tuple{right_number, right_text, right_tail};

        check_values(left, standard_left, i);
        check_values(right, standard_right, i);
        check_relations(left, right, standard_left, standard_right, i);
    }
}

} // namespace

int main() {
    default_and_special_members();
    integer_stress();
    heterogeneous_stress();
    std::cout << "Tuple stress test passed\n";
}
