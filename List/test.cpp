// Build from the repository root, then run /tmp/list-tests:
// clang++ -std=c++20 -O1 -g -fsanitize=address,undefined
//   -fno-sanitize-recover=all List/test.cpp -o /tmp/list-tests
// Runtime tests use child processes on macOS/Linux to isolate crashes.
#include "List.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <list>
#include <random>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <stdexcept>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

using lam::List;

static_assert(
    std::is_same_v<
        decltype(std::declval<List<int>&>() = std::declval<const List<int>&>()),
        List<int>&>,
    "copy assignment must return List&");

static_assert(
    std::is_same_v<
        decltype(std::declval<List<int>&>() = std::declval<List<int>&&>()),
        List<int>&>,
    "move assignment must return List&");

// Moves may either avoid throwing operations or propagate exceptions. Do not
// require noexcept without verifying that the implementation can honor it.

namespace {

[[noreturn]] void fail(std::string_view check,
                       std::size_t step,
                       std::size_t index = 0) {
    std::cerr << "FAIL: " << check << " (step=" << step
              << ", index=" << index << ")\n";
    std::abort();
}

template<typename Container>
auto iterator_at(Container& container, std::size_t index) {
    auto iterator = container.begin();
    while (index > 0) {
        ++iterator;
        --index;
    }
    return iterator;
}

void verify(List<int>& actual,
            const std::list<int>& expected,
            std::size_t step) {
    if (actual.empty() != expected.empty()) {
        fail("empty", step);
    }
    if (actual.size() != expected.size()) {
        fail("size", step);
    }

    if (!expected.empty()) {
        if (actual.front() != expected.front()) {
            fail("front", step);
        }
        if (actual.back() != expected.back()) {
            fail("back", step);
        }
    }

    auto actual_iterator = actual.begin();
    auto expected_iterator = expected.begin();
    std::size_t index = 0;

    while (expected_iterator != expected.end()) {
        if (actual_iterator == actual.end()) {
            fail("early end", step, index);
        }
        if (*actual_iterator != *expected_iterator) {
            fail("iterator value", step, index);
        }
        ++actual_iterator;
        ++expected_iterator;
        ++index;
    }

    if (actual_iterator != actual.end()) {
        fail("late end", step, index);
    }
}

void constructor_tests() {
    List<int> initializer_list{1, 2, 3, 4, 5};
    verify(initializer_list, std::list<int>{1, 2, 3, 4, 5}, 0);

    const std::vector<int> source{9, 8, 7, 6};
    List<int> from_container{source};
    verify(from_container, std::list<int>{9, 8, 7, 6}, 0);

    List<int> empty;
    verify(empty, std::list<int>{}, 0);
}

void deterministic_tests() {
    List<int> actual{2, 4};
    std::list<int> expected{2, 4};

    int one = 1;
    auto inserted = actual.insert(actual.begin(), one);
    expected.insert(expected.begin(), one);
    if (*inserted != one) {
        fail("insert lvalue return", 1);
    }

    inserted = actual.insert(iterator_at(actual, 2), 3);
    expected.insert(iterator_at(expected, 2), 3);
    if (*inserted != 3) {
        fail("insert rvalue return", 2);
    }

    actual.insert(actual.end(), 5);
    expected.insert(expected.end(), 5);
    verify(actual, expected, 3);

    auto actual_next = actual.erase(actual.begin());
    auto expected_next = expected.erase(expected.begin());
    if (*actual_next != *expected_next) {
        fail("erase first return", 4);
    }

    actual_next = actual.erase(iterator_at(actual, 1));
    expected_next = expected.erase(iterator_at(expected, 1));
    if (*actual_next != *expected_next) {
        fail("erase middle return", 5);
    }

    actual_next = actual.erase(iterator_at(actual, actual.size() - 1));
    expected_next = expected.erase(iterator_at(expected, expected.size() - 1));
    if ((actual_next == actual.end()) != (expected_next == expected.end())) {
        fail("erase last return", 6);
    }

    verify(actual, expected, 7);
}

void randomized_stress() {
    constexpr std::size_t operations = 200'000;
    constexpr std::uint64_t seed = 0x1157'2026'57AULL;

    std::mt19937_64 random(seed);
    std::uniform_int_distribution<int> value_distribution(-1'000'000'000,
                                                           1'000'000'000);
    std::uniform_int_distribution<int> operation_distribution(0, 6);

    List<int> actual;
    std::list<int> expected;

    struct IteratorPair {
        List<int>::iterator actual;
        std::list<int>::iterator expected;
    };

    std::vector<IteratorPair> elements;

    for (std::size_t step = 0; step < operations; ++step) {
        const int value = value_distribution(random);
        const int operation = operation_distribution(random);

        if (operation <= 2 || elements.empty()) {
            auto actual_position = actual.end();
            auto expected_position = expected.end();

            if (!elements.empty()) {
                const std::size_t position = static_cast<std::size_t>(
                    random() % (elements.size() + 1));
                if (position < elements.size()) {
                    actual_position = elements[position].actual;
                    expected_position = elements[position].expected;
                }
            }

            auto actual_inserted = operation % 2 == 0
                ? actual.insert(actual_position, value)
                : actual.insert(actual_position, int{value});
            auto expected_inserted = expected.insert(expected_position, value);

            if (*actual_inserted != *expected_inserted) {
                fail("random insert return", step);
            }

            elements.push_back({actual_inserted, expected_inserted});
        } else if (operation <= 5) {
            const std::size_t selected =
                static_cast<std::size_t>(random() % elements.size());
            auto actual_next = actual.erase(elements[selected].actual);
            auto expected_next = expected.erase(elements[selected].expected);

            const bool actual_is_end = actual_next == actual.end();
            const bool expected_is_end = expected_next == expected.end();
            if (actual_is_end != expected_is_end) {
                fail("random erase return end", step, selected);
            }
            if (!actual_is_end && *actual_next != *expected_next) {
                fail("random erase return value", step, selected);
            }

            elements[selected] = elements.back();
            elements.pop_back();
        } else {
            const std::size_t selected =
                static_cast<std::size_t>(random() % elements.size());
            *elements[selected].actual = value;
            *elements[selected].expected = value;
        }

        if ((step + 1) % 4096 == 0) {
            verify(actual, expected, step);
            if (elements.size() != actual.size()) {
                fail("tracked iterator count", step, elements.size());
            }
        }
    }

    verify(actual, expected, operations);
}

void copy_independence_test() {
    List<int> original{1, 2, 3};
    List<int> copy{original};

    *original.begin() = 99;
    verify(copy, std::list<int>{1, 2, 3}, 0);

    *copy.begin() = -1;
    verify(original, std::list<int>{99, 2, 3}, 0);
}

void copy_assignment_tests() {
    List<int> self{1, 2, 3};
    List<int>* self_address = &(self = self);
    if (self_address != &self) {
        fail("copy assignment return", 0);
    }
    verify(self, std::list<int>{1, 2, 3}, 0);

    List<int> source{4, 5, 6, 7};
    List<int> destination{90, 91, 92};
    List<int>* destination_address = &(destination = source);
    if (destination_address != &destination) {
        fail("copy assignment destination", 0);
    }
    verify(destination, std::list<int>{4, 5, 6, 7}, 0);

    *source.begin() = 400;
    verify(destination, std::list<int>{4, 5, 6, 7}, 0);

    destination.push_back(8);
    verify(destination, std::list<int>{4, 5, 6, 7, 8}, 0);
}

void move_constructor_tests() {
    List<int> source{10, 20, 30};
    List<int> destination{std::move(source)};

    verify(destination, std::list<int>{10, 20, 30}, 0);
    if (!source.empty() || source.size() != 0) {
        fail("move constructor source state", 0);
    }

    source.push_back(40);
    verify(source, std::list<int>{40}, 0);
}

void move_assignment_tests() {
    List<int> source{10, 20, 30};
    List<int> destination{70, 80};

    List<int>* destination_address = &(destination = std::move(source));
    if (destination_address != &destination) {
        fail("move assignment return", 0);
    }

    verify(destination, std::list<int>{10, 20, 30}, 0);
    if (!source.empty() || source.size() != 0) {
        fail("move assignment source state", 0);
    }

    source.push_front(5);
    verify(source, std::list<int>{5}, 0);

    destination = std::move(destination);
    destination.push_back(40);
}

struct Tracked {
    static inline std::size_t alive = 0;

    int value = 0;

    Tracked() { ++alive; }
    explicit Tracked(int number) : value{number} { ++alive; }
    Tracked(const Tracked& other) : value{other.value} { ++alive; }
    Tracked(Tracked&& other) noexcept : value{other.value} { ++alive; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --alive; }
};

void lifetime_test() {
    if (Tracked::alive != 0) {
        fail("tracked initial lifetime", 0);
    }

    {
        List<Tracked> values;
        for (int i = 0; i < 10'000; ++i) {
            values.push_back(Tracked{i});
        }
    }

    if (Tracked::alive != 0) {
        fail("tracked final lifetime", 0, Tracked::alive);
    }
}

#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (false)

void empty_list_has_no_elements() {
    CHECK(Tracked::alive == 0);
    {
        List<Tracked> values;
        CHECK(values.empty());
        CHECK(Tracked::alive == 0); // A sentinel must not construct a T.
        values.push_back(Tracked{7});
        CHECK(Tracked::alive == values.size());
        values.erase(values.begin());
        CHECK(values.empty());
        CHECK(Tracked::alive == 0);
    }
    CHECK(Tracked::alive == 0);
}

void const_access_is_read_only() {
    using C = List<int>;
    constexpr bool readonly_iterator = !std::is_assignable_v<
        decltype(*std::declval<const C&>().begin()), int>;
    constexpr bool readonly_front = !std::is_assignable_v<
        decltype(std::declval<const C&>().front()), int>;
    constexpr bool readonly_back = !std::is_assignable_v<
        decltype(std::declval<const C&>().back()), int>;
    CHECK(readonly_iterator);
    CHECK(readonly_front);
    CHECK(readonly_back);
    C values{1, 2};
    values.front() = 3;
    values.back() = 4;
    const C& view = values;
    CHECK(view.front() == 3);
    CHECK(view.back() == 4);
}

struct ThrowingDefault {
    static inline bool fail_default = false;
    int value = 0;
    ThrowingDefault() {
        if (fail_default) throw std::runtime_error("injected default-constructor failure");
    }
    explicit ThrowingDefault(int n) : value(n) {}
};

template<bool Assign>
void move_does_not_terminate() {
    List<ThrowingDefault> source;
    source.push_back(ThrowingDefault{7});
    List<ThrowingDefault> destination;
    destination.push_back(ThrowingDefault{9});
    ThrowingDefault::fail_default = true;
    bool threw = false;
    try {
        if constexpr (Assign) {
            destination = std::move(source);
            CHECK(destination.size() == 1);
            CHECK(destination.front().value == 7);
        } else {
            List<ThrowingDefault> moved(std::move(source));
            CHECK(moved.size() == 1);
            CHECK(moved.front().value == 7);
        }
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ThrowingDefault::fail_default = false;
    // Either avoid default-constructing T during move or propagate its failure.
    // Calling terminate through an incorrect noexcept declaration is a failure.
    if (threw) {
        CHECK(source.size() == 1);
        CHECK(source.front().value == 7);
        CHECK(destination.front().value == 9);
    }
    source.push_back(ThrowingDefault{8});
    CHECK(source.back().value == 8);
}

struct ThrowingCopy {
    static inline int alive = 0;
    static inline int copies = 0;
    static inline int fail_copy = 0;
    int value = 0;
    ThrowingCopy() { ++alive; }
    explicit ThrowingCopy(int n) : value(n) { ++alive; }
    ThrowingCopy(const ThrowingCopy& other) : value(other.value) {
        if (++copies == fail_copy) throw std::runtime_error("injected copy failure");
        ++alive;
    }
    ThrowingCopy(ThrowingCopy&& other) noexcept : value(other.value) { ++alive; }
    ~ThrowingCopy() { --alive; }
};

void failed_copy_cleans_up() {
    CHECK(ThrowingCopy::alive == 0);
    {
        List<ThrowingCopy> source;
        source.push_back(ThrowingCopy{1});
        source.push_back(ThrowingCopy{2});
        const int before = ThrowingCopy::alive;
        ThrowingCopy::copies = 0;
        ThrowingCopy::fail_copy = 2;
        bool threw = false;
        try { List<ThrowingCopy> copy(source); }
        catch (const std::runtime_error&) { threw = true; }
        ThrowingCopy::fail_copy = 0;
        CHECK(threw);
        CHECK(ThrowingCopy::alive == before);
        CHECK(source.size() == 2);
        CHECK(source.front().value == 1);
        CHECK(source.back().value == 2);
    }
    CHECK(ThrowingCopy::alive == 0);
}

void failed_insert_preserves_links() {
    List<ThrowingCopy> values;
    values.push_back(ThrowingCopy{1});
    values.push_back(ThrowingCopy{2});
    ThrowingCopy candidate{3};
    const int before = ThrowingCopy::alive;
    ThrowingCopy::copies = 0;
    ThrowingCopy::fail_copy = 1;
    bool threw = false;
    try { values.insert(iterator_at(values, 1), candidate); }
    catch (const std::runtime_error&) { threw = true; }
    ThrowingCopy::fail_copy = 0;
    CHECK(threw);
    CHECK(ThrowingCopy::alive == before);
    CHECK(values.size() == 2);
    CHECK(values.front().value == 1);
    CHECK(values.back().value == 2);
    CHECK((*iterator_at(values, 1)).value == 2);
    CHECK(iterator_at(values, 2) == values.end());
}

struct NonDefault {
    NonDefault() = delete;
    explicit NonDefault(int n) : value(n) {}
    int value;
};
void non_default_constructible_elements() {
    List<NonDefault> values;
    values.push_back(NonDefault{7});
    CHECK(values.front().value == 7);
    values.erase(values.begin());
    CHECK(values.empty());
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
        alarm(30);
        int result = 0;
        try { function(); }
        catch (const std::exception& error) {
            std::cerr << "[DETAIL] " << name << ": " << error.what() << '\n';
            result = 1;
        }
        catch (...) { result = 1; }
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(result);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    const bool passed = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name;
    if (waited == child && WIFSIGNALED(status)) std::cout << " (signal " << WTERMSIG(status) << ')';
    std::cout << '\n';
    return passed;
}

} // namespace

int main() {
    int failures = 0;
    failures += !run_test("constructors", constructor_tests);
    failures += !run_test("deterministic operations", deterministic_tests);
    failures += !run_test("randomized differential", randomized_stress);
    failures += !run_test("copy independence", copy_independence_test);
    failures += !run_test("copy assignment", copy_assignment_tests);
    failures += !run_test("move construction", move_constructor_tests);
    failures += !run_test("move assignment", move_assignment_tests);
    failures += !run_test("element lifetime cleanup", lifetime_test);
    failures += !run_test("empty list constructs no elements", empty_list_has_no_elements);
    failures += !run_test("const access", const_access_is_read_only);
    failures += !run_test("move constructor exception", move_does_not_terminate<false>);
    failures += !run_test("move assignment exception", move_does_not_terminate<true>);
    failures += !run_test("failed copy cleanup", failed_copy_cleans_up);
    failures += !run_test("failed insertion preserves links", failed_insert_preserves_links);
    failures += !run_test("non-default-constructible elements", non_default_constructible_elements);
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All enabled List tests passed\n";
}
