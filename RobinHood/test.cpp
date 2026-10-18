// Full compile check (includes instantiating erase(iterator)):
//   clang++ -std=c++20 -O2 -g -fsanitize=address,undefined
//     -fno-sanitize-recover=all RobinHood/test.cpp -o /tmp/robinhood-tests
// Run /tmp/robinhood-tests after a successful build.
// Use -O2 for the deliberately long, constant-hash collision regression.
#include "RobinHood.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <type_traits>
#include <cstdlib>
#include <cerrno>

// Each test runs in a child process so sanitizer failures and signals do not
// prevent the remaining regressions from running (macOS/Linux).
#include <sys/wait.h>
#include <unistd.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using lam::RobinHood;

namespace {

[[noreturn]] void fail(
    const char* expression,
    const char* file,
    int line
) {
    std::ostringstream message;
    message << file << ':' << line << ": check failed: " << expression;
    throw std::runtime_error(message.str());
}

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fail(#expression, __FILE__, __LINE__);                            \
        }                                                                     \
    } while (false)

struct ConstantHash {
    std::size_t operator()(int) const noexcept {
        return 0;
    }
};

struct EndBucketHash {
    std::size_t operator()(int) const noexcept {
        return 63;
    }
};

template<class Hash>
void check_equal(
    RobinHood<int, int, Hash>& actual,
    const std::unordered_map<int, int>& expected
) {
    CHECK(actual.size() == expected.size());
    CHECK(actual.empty() == expected.empty());

    std::unordered_set<int> visited;
    for (auto entry : actual) {
        const auto expected_it = expected.find(entry.first);
        CHECK(expected_it != expected.end());
        if (entry.second != expected_it->second) {
            std::ostringstream message;
            message << "value mismatch for key " << entry.first
                    << ": RobinHood=" << entry.second
                    << ", unordered_map=" << expected_it->second;
            throw std::runtime_error(message.str());
        }
        CHECK(visited.insert(entry.first).second);
    }
    CHECK(visited.size() == expected.size());

    for (const auto& [key, value] : expected) {
        auto found = actual.find(key);
        CHECK(found != actual.end());
        CHECK(found->first == key);
        CHECK(found->second == value);
    }
}

void test_basic_operations() {
    RobinHood<int, int> map;

    CHECK(map.empty());
    CHECK(map.size() == 0);
    CHECK(map.find(123) == map.end());
    CHECK(map.erase(123) == 0);

    CHECK(map[42] == 0);
    CHECK(map.size() == 1);

    map[42] = 99;
    CHECK(map[42] == 99);
    CHECK(map.size() == 1);

    map[-7] = 11;
    CHECK(map.find(-7) != map.end());
    CHECK(map.find(-7)->second == 11);
    CHECK(map.erase(-7) == 1);
    CHECK(map.erase(-7) == 0);
    CHECK(map.find(-7) == map.end());
    CHECK(map.size() == 1);
}

void test_duplicate_insert_does_not_grow() {
    RobinHood<int, int> map;

    map.insert({7, 10});
    const std::size_t size_before = map.size();
    map.insert({7, 20});

    CHECK(map.size() == size_before);
    CHECK(map.find(7) != map.end());
}

void test_rehash_and_large_sequential_workload() {
    RobinHood<int, int> map;
    std::unordered_map<int, int> expected;

    constexpr int count = 20'000;
    for (int key = 0; key < count; ++key) {
        map[key] = key * 3 + 1;
        expected[key] = key * 3 + 1;
    }
    check_equal(map, expected);

    for (int key = 0; key < count; key += 3) {
        CHECK(map.erase(key) == expected.erase(key));
    }
    check_equal(map, expected);

    for (int key = 0; key < count; key += 5) {
        map[key] = -key;
        expected[key] = -key;
    }
    check_equal(map, expected);
}

void test_long_collision_chain() {
    RobinHood<int, int, ConstantHash> map;
    std::unordered_map<int, int> expected;

    constexpr int count = 180;
    for (int key = 0; key < count; ++key) {
        map[key] = key + 1000;
        expected[key] = key + 1000;
    }
    check_equal(map, expected);

    for (int key = 0; key < count; key += 2) {
        CHECK(map.erase(key) == expected.erase(key));
    }
    check_equal(map, expected);

    for (int key = count; key < count + 60; ++key) {
        map[key] = key + 1000;
        expected[key] = key + 1000;
    }
    check_equal(map, expected);
}

void test_collision_chain_wraps_around() {
    RobinHood<int, int, EndBucketHash> map(64);
    std::unordered_map<int, int> expected;

    for (int key = 0; key < 40; ++key) {
        map[key] = key * key;
        expected[key] = key * key;
    }
    check_equal(map, expected);

    for (int key = 5; key < 30; ++key) {
        CHECK(map.erase(key) == expected.erase(key));
    }
    check_equal(map, expected);
}

void test_copy_and_move() {
    RobinHood<int, int> source;
    std::unordered_map<int, int> expected;

    for (int key = -500; key <= 500; ++key) {
        source[key] = key * 17;
        expected[key] = key * 17;
    }

    RobinHood<int, int> copied(source);
    check_equal(copied, expected);

    RobinHood<int, int> copy_assigned;
    copy_assigned[999'999] = 1;
    copy_assigned = source;
    check_equal(copy_assigned, expected);

    RobinHood<int, int> moved(std::move(copied));
    check_equal(moved, expected);

    RobinHood<int, int> move_assigned;
    move_assigned[-999'999] = 1;
    move_assigned = std::move(copy_assigned);
    check_equal(move_assigned, expected);
}

void test_randomized_against_unordered_map() {
    RobinHood<int, int> actual;
    std::unordered_map<int, int> expected;

    std::mt19937_64 random(0xD1FF'3A5E'BADC'0FFEULL);
    std::uniform_int_distribution<int> operation(0, 99);
    std::uniform_int_distribution<int> keys(-2'000, 2'000);
    std::uniform_int_distribution<int> values(-1'000'000, 1'000'000);

    constexpr int operation_count = 100'000;
    for (int step = 0; step < operation_count; ++step) {
        const int key = keys(random);
        const int op = operation(random);

        if (op < 50) {
            const int value = values(random);
            actual[key] = value;
            expected[key] = value;
        } else if (op < 70) {
            CHECK(actual.erase(key) == expected.erase(key));
        } else if (op < 90) {
            auto actual_it = actual.find(key);
            auto expected_it = expected.find(key);
            CHECK((actual_it == actual.end()) ==
                  (expected_it == expected.end()));
            if (expected_it != expected.end()) {
                CHECK(actual_it->second == expected_it->second);
            }
        } else {
            CHECK(actual[key] == expected[key]);
        }

        if (step % 257 == 0) {
            check_equal(actual, expected);
        }
    }

    check_equal(actual, expected);
}

struct Tracked {
    static inline int alive = 0;

    int value = 0;

    Tracked() {
        ++alive;
    }

    explicit Tracked(int initial_value) : value(initial_value) {
        ++alive;
    }

    Tracked(const Tracked& other) : value(other.value) {
        ++alive;
    }

    Tracked(Tracked&& other) noexcept : value(other.value) {
        ++alive;
        other.value = -1;
    }

    Tracked& operator=(const Tracked&) = default;

    Tracked& operator=(Tracked&& other) noexcept {
        value = other.value;
        other.value = -1;
        return *this;
    }

    ~Tracked() {
        --alive;
    }
};

void test_non_trivial_value_lifetimes() {
    CHECK(Tracked::alive == 0);

    {
        RobinHood<int, Tracked, ConstantHash> map;

        for (int key = 0; key < 120; ++key) {
            map[key] = Tracked(key);
        }
        for (int key = 0; key < 120; key += 3) {
            CHECK(map.erase(key) == 1);
        }

        RobinHood<int, Tracked, ConstantHash> copy(map);
        CHECK(copy.size() == map.size());

        for (int key = 0; key < 120; ++key) {
            auto found = copy.find(key);
            if (key % 3 == 0) {
                CHECK(found == copy.end());
            } else {
                CHECK(found != copy.end());
                CHECK(found->second.value == key);
            }
        }
    }

    CHECK(Tracked::alive == 0);
}

void test_erase_iterator() {
    RobinHood<int, int, ConstantHash> map;
    map.insert({1, 10});
    map.insert({2, 20});
    map.insert({3, 30});

    auto next = map.erase(map.find(2));
    CHECK(map.size() == 2);
    CHECK(map.find(2) == map.end());
    CHECK(next != map.end());
    CHECK(next->first == 3);
    CHECK(next->second == 30);

    next = map.erase(next);
    CHECK(next == map.end());
    CHECK(map.size() == 1);
    CHECK(map.erase(map.begin()) == map.end());
    CHECK(map.empty());
}

template<class Map>
void check_postfix_increment() {
    using Iterator = decltype(std::declval<Map&>().begin());
    // Reject a dangling-reference return without actually dereferencing it.
    constexpr bool returns_value =
        std::is_same_v<decltype(std::declval<Iterator&>()++), Iterator>;
    CHECK(returns_value);
    if constexpr (returns_value) {
        Map map;
        map.insert({1, 10});
        map.insert({2, 20});
        auto it = map.begin();
        auto first = it;
        auto old = it++;
        CHECK(old == first);
        CHECK(old->first == 1);
        CHECK(it->first == 2);
        auto last = it;
        old = it++;
        CHECK(old == last);
        CHECK(old->first == 2);
        CHECK(it == map.end());
    }
}

void test_postfix_increment() {
    check_postfix_increment<RobinHood<int, int>>();
}

struct ThrowingValue {
    static inline bool throw_on_move = false;
    static inline int alive = 0;
    ThrowingValue() { ++alive; }
    ThrowingValue(const ThrowingValue&) { ++alive; }
    ThrowingValue(ThrowingValue&&) {
        if (throw_on_move) throw std::runtime_error("injected move failure");
        ++alive;
    }
    ThrowingValue& operator=(const ThrowingValue&) = default;
    ThrowingValue& operator=(ThrowingValue&&) = default;
    ~ThrowingValue() { --alive; }
};

template<class Key, class Value, class Hash = std::hash<Key>>
concept SupportsRobinHood = requires { typename RobinHood<Key, Value, Hash>; };

struct ThrowingHash {
    std::size_t operator()(int key) const { return static_cast<std::size_t>(key); }
};

void test_type_constraints() {
    static_assert(SupportsRobinHood<int, int>);
    static_assert(SupportsRobinHood<int, Tracked, ConstantHash>);
    static_assert(!SupportsRobinHood<int, ThrowingValue>);
    static_assert(!SupportsRobinHood<int, int, ThrowingHash>);
}

struct alignas(4096) OverAlignedValue {
    int value = 0;
};

void test_over_aligned_values() {
    RobinHood<int, OverAlignedValue> map;
    // Cross several allocations, including rehashes. UBSan also checks the
    // construction itself, before the explicit address check can run.
    for (int key = 0; key < 40; ++key) {
        auto& value = map[key];
        CHECK(reinterpret_cast<std::uintptr_t>(std::addressof(value)) %
              alignof(OverAlignedValue) == 0);
        value.value = key + 1;
    }
    for (int key = 0; key < 40; ++key) {
        auto& value = map.find(key)->second;
        CHECK(reinterpret_cast<std::uintptr_t>(std::addressof(value)) %
              alignof(OverAlignedValue) == 0);
        CHECK(value.value == key + 1);
    }
}

void test_probe_distance_overflow() {
    constexpr int count = static_cast<int>(
        std::numeric_limits<std::uint16_t>::max()) + 1;
    RobinHood<int, int, ConstantHash> map(131'072);
    int inserted = 0;
    try {
        for (; inserted < count; ++inserted) {
            map.insert({inserted, inserted});
        }
    } catch (const std::length_error&) {
        // Explicitly rejecting an unsupported probe length is also valid,
        // provided previously inserted entries remain intact.
    }
    CHECK(map.size() == static_cast<std::size_t>(inserted));
    if (inserted != 0) {
        CHECK(map.find(inserted - 1) != map.end());
    }
    // Iteration validates the entire chain in O(n), rather than O(n^2) finds.
    std::unordered_set<int> visited;
    for (auto entry : map) {
        CHECK(entry.first >= 0 && entry.first < inserted);
        CHECK(entry.second == entry.first);
        CHECK(visited.insert(entry.first).second);
    }
    CHECK(visited.size() == map.size());
    CHECK(map.find(-1) == map.end());
}

void test_zero_capacity() {
    std::unique_ptr<RobinHood<int, int>> map;
    try {
        map = std::make_unique<RobinHood<int, int>>(0);
    } catch (const std::invalid_argument&) {
        return; // Rejecting zero and normalizing it are both valid policies.
    } catch (const std::length_error&) {
        return;
    }
    CHECK(map->empty());
    CHECK(map->find(1) == map->end());
    (*map)[1] = 42;
    CHECK(map->size() == 1);
    CHECK(map->find(1)->second == 42);
    CHECK(map->erase(1) == 1);
    CHECK(map->empty());
}

void test_const_access_is_read_only() {
    using Map = RobinHood<int, int>;
    using ConstIterator = decltype(std::declval<const Map&>().begin());
    using ConstFindIterator = decltype(std::declval<const Map&>().find(0));
    using Iterator = decltype(std::declval<Map&>().begin());
    constexpr bool const_begin_read_only = !std::is_assignable_v<
        decltype((std::declval<ConstIterator&>()->second)), int>;
    constexpr bool const_find_read_only = !std::is_assignable_v<
        decltype((std::declval<ConstFindIterator&>()->second)), int>;
    constexpr bool const_deref_read_only = !std::is_assignable_v<
        decltype(((*std::declval<ConstIterator&>()).second)), int>;
    constexpr bool mutable_value_writable = std::is_assignable_v<
        decltype((std::declval<Iterator&>()->second)), int>;
    CHECK(const_begin_read_only);
    CHECK(const_find_read_only);
    CHECK(const_deref_read_only);
    CHECK(mutable_value_writable);
    static_assert(std::is_convertible_v<Iterator, ConstIterator>);
    static_assert(!std::is_convertible_v<ConstIterator, Iterator>);
    Map map;
    map[1] = 42;
    const Map& readonly = map;
    ConstIterator converted = map.begin();
    CHECK(converted == readonly.begin());
    CHECK(map.begin() == converted);
    CHECK(converted == map.begin());
    CHECK(readonly.find(1)->second == 42);
    CHECK(readonly.cbegin() == readonly.begin());
    CHECK(readonly.cend() == readonly.end());
    CHECK(++converted == readonly.end());
}

template<class Function>
bool run_test(const char* name, Function function) {
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "[FAIL] " << name << ": fork failed\n";
        return false;
    }
    if (child == 0) {
        alarm(60); // Also catch infinite rehash/probe loops.
        int result = 0;
        try {
            function();
        } catch (const std::exception& error) {
            std::cerr << "[DETAIL] " << name << ": " << error.what() << '\n';
            result = 1;
        } catch (...) {
            std::cerr << "[DETAIL] " << name << ": unknown exception\n";
            result = 1;
        }
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(result);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const bool passed = waited == child && WIFEXITED(status) &&
                        WEXITSTATUS(status) == 0;
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name;
    if (waited == child && WIFSIGNALED(status)) {
        std::cout << " (signal " << WTERMSIG(status) << ')';
    }
    std::cout << '\n';
    return passed;
}

} // namespace

int main() {
    int failures = 0;

    failures += !run_test("basic operations", test_basic_operations);
    failures += !run_test(
        "duplicate insertion does not grow",
        test_duplicate_insert_does_not_grow
    );
    failures += !run_test(
        "rehash and large sequential workload",
        test_rehash_and_large_sequential_workload
    );
    failures += !run_test(
        "long collision chain",
        test_long_collision_chain
    );
    failures += !run_test(
        "collision chain wraps around",
        test_collision_chain_wraps_around
    );
    failures += !run_test("copy and move", test_copy_and_move);
    failures += !run_test(
        "randomized differential test",
        test_randomized_against_unordered_map
    );
    failures += !run_test(
        "non-trivial value lifetimes",
        test_non_trivial_value_lifetimes
    );

    failures += !run_test("postfix increment", test_postfix_increment);
    failures += !run_test("erase by iterator", test_erase_iterator);
    failures += !run_test("non-throwing type constraints", test_type_constraints);
    failures += !run_test("over-aligned values", test_over_aligned_values);
    failures += !run_test("probe distance overflow", test_probe_distance_overflow);
    failures += !run_test("zero capacity", test_zero_capacity);
    failures += !run_test("const access is read-only", test_const_access_is_read_only);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All enabled RobinHood tests passed\n";
}
