// Build from the repository root, then run /tmp/circular-queue-tests:
// clang++ -std=c++20 -O1 -g -fsanitize=address,undefined
//   -fno-sanitize-recover=all CircularQueue/test.cpp -o /tmp/circular-queue-tests
// Tests run in separate child processes (macOS/Linux), so a crash or sanitizer
// failure does not prevent the remaining regressions from running.
#include "CircularQueue.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <random>
#include <string_view>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

using lam::CircularQueue;

namespace {

[[noreturn]] void fail(std::string_view check,
                       std::size_t step,
                       std::size_t index = 0) {
    std::cerr << "FAIL: " << check << " (step=" << step
              << ", index=" << index << ")\n";
    std::abort();
}

void verify(CircularQueue<int>& actual,
            const std::deque<int>& expected,
            std::size_t step) {
    if (actual.empty() != expected.empty()) {
        fail("empty", step);
    }

    if (actual.size() != expected.size()) {
        fail("size", step);
    }

    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            fail("operator[]", step, i);
        }
    }

    std::size_t i = 0;
    for (int value : actual) {
        if (i >= expected.size() || value != expected[i]) {
            fail("iterator", step, i);
        }
        ++i;
    }

    if (i != expected.size()) {
        fail("iterator count", step, i);
    }
}

void deterministic_stress() {
    CircularQueue<int> actual;
    std::deque<int> expected;

    constexpr std::size_t operations = 100'000;
    for (std::size_t step = 0; step < operations; ++step) {
        const int value = static_cast<int>(step);

        switch (step % 4) {
        case 0:
            actual.push_back(value);
            expected.push_back(value);
            break;
        case 1:
            actual.push_front(value);
            expected.push_front(value);
            break;
        case 2: {
            const int lvalue = -value;
            actual.push_back(lvalue);
            expected.push_back(lvalue);
            break;
        }
        default: {
            int& inserted = actual.emplace_front(-value);
            expected.emplace_front(-value);
            if (inserted != expected.front()) {
                fail("emplace_front return", step);
            }
            break;
        }
        }

        if ((step + 1) % 4096 == 0) {
            verify(actual, expected, step);
        }
    }

    verify(actual, expected, operations);
    if (actual.front() != expected.front()) {
        fail("front", operations);
    }
    if (actual.back() != expected.back()) {
        fail("back", operations);
    }
}

void randomized_stress() {
    constexpr std::uint64_t seed = 0xC1AC'0A11'2026ULL;
    constexpr std::size_t operations = 250'000;

    std::mt19937_64 random(seed);
    std::uniform_int_distribution<int> value_distribution(-1'000'000'000,
                                                           1'000'000'000);
    std::uniform_int_distribution<int> operation_distribution(0, 3);

    CircularQueue<int> actual;
    std::deque<int> expected;

    for (std::size_t step = 0; step < operations; ++step) {
        const int value = value_distribution(random);

        switch (operation_distribution(random)) {
        case 0:
            actual.push_front(value);
            expected.push_front(value);
            break;
        case 1:
            actual.push_back(value);
            expected.push_back(value);
            break;
        case 2: {
            int& inserted = actual.emplace_front(value);
            expected.emplace_front(value);
            if (inserted != expected.front()) {
                fail("random emplace_front return", step);
            }
            break;
        }
        default: {
            int& inserted = actual.emplace_back(value);
            expected.emplace_back(value);
            if (inserted != expected.back()) {
                fail("random emplace_back return", step);
            }
            break;
        }
        }

        if ((step + 1) % 8192 == 0) {
            verify(actual, expected, step);
        }
    }

    verify(actual, expected, operations);
    if (actual.front() != expected.front()) {
        fail("random front", operations);
    }
    if (actual.back() != expected.back()) {
        fail("random back", operations);
    }
}

#define CHECK(condition) do { if (!(condition)) fail(#condition, __LINE__); } while (false)

struct Tracked {
    static inline std::unordered_set<const Tracked*> live;
    static inline bool throw_constructor = false;
    static inline bool throw_move = false;
    static inline int copies = 0;
    static inline int fail_copy = 0;
    int value;

    explicit Tracked(int v = 0) : value(v) {
        if (throw_constructor) throw std::runtime_error("injected constructor failure");
        live.insert(this);
    }
    Tracked(const Tracked& other) : value(other.value) {
        if (++copies == fail_copy) throw std::runtime_error("injected copy failure");
        live.insert(this);
    }
    Tracked(Tracked&& other) noexcept(false) : value(other.value) {
        if (throw_move) throw std::runtime_error("injected move failure");
        live.insert(this);
    }
    ~Tracked() { CHECK(live.erase(this) == 1); }
};

void destruction_releases_elements() {
    CHECK(Tracked::live.empty());
    {
        CircularQueue<Tracked> q;
        q.emplace_front(1);
        q.emplace_back(2);
        q.emplace_front(3);
        CHECK(Tracked::live.size() == q.size());
    }
    CHECK(Tracked::live.empty());
}

template<bool Assign>
void copying_is_independent() {
    CircularQueue<int> source;
    source.push_back(7);
    CircularQueue<int> copy = [&] {
        if constexpr (Assign) {
            CircularQueue<int> target;
            target.push_back(100);
            target = source;
            return target;
        } else {
            return CircularQueue<int>(source);
        }
    }();
    CHECK(copy.size() == 1);
    CHECK(copy[0] == 7);
    copy[0] = 99;
    CHECK(source[0] == 7);
    source.reserve(16);
    CHECK(copy[0] == 99);
    copy.push_front(8);
    CHECK(source.size() == 1);
}

template<bool Assign>
void moving_preserves_ownership() {
    std::unique_ptr<CircularQueue<int>> destination;
    {
        CircularQueue<int> source;
        source.push_back(7);
        if constexpr (Assign) {
            destination = std::make_unique<CircularQueue<int>>();
            destination->push_back(100);
            *destination = std::move(source);
        } else {
            destination = std::make_unique<CircularQueue<int>>(std::move(source));
        }
        CHECK(destination->size() == 1);
        // A moved-from queue must remain usable, without freeing or modifying
        // the destination's storage. No particular moved-from size is assumed.
        source.push_back(42);
        source.reserve(16);
        CHECK((*destination)[0] == 7);
    }
    CHECK((*destination)[0] == 7);
    destination->push_front(9);
    CHECK(destination->front() == 9);
}

void reserve_does_not_shrink() {
    CircularQueue<int> q;
    q.push_back(1);
    q.push_back(2);
    q.reserve(1);
    verify(q, {1, 2}, 0);
    q.push_front(3);
    verify(q, {3, 1, 2}, 1);
}

void reserve_zero_is_safe() {
    CircularQueue<int> q;
    q.reserve(0);
    q.push_back(1);
    verify(q, {1}, 0);
    q.reserve(0);
    verify(q, {1}, 1);
}

void failed_reserve_preserves_objects() {
    CircularQueue<Tracked> q;
    q.reserve(4);
    q.emplace_front(1);
    q.emplace_front(2); // wrapped storage, logical order is [2, 1]
    const auto* first = std::addressof(q[0]);
    const auto* second = std::addressof(q[1]);
    const auto live_before = Tracked::live;
    Tracked::copies = 0;
    Tracked::fail_copy = 2;
    bool threw = false;
    try { q.reserve(8); } catch (const std::runtime_error&) { threw = true; }
    Tracked::fail_copy = 0;
    CHECK(threw);
    // Check lifetimes before reading elements that a buggy reserve destroyed.
    CHECK(Tracked::live.contains(first));
    CHECK(Tracked::live.contains(second));
    CHECK(Tracked::live == live_before);
    CHECK(q.size() == 2);
    CHECK(q[0].value == 2 && q[1].value == 1);
    q.reserve(8);
    CHECK(q[0].value == 2 && q[1].value == 1);
}

template<bool Front, bool Grow>
void failed_insertion_preserves_state() {
    CircularQueue<Tracked> q;
    if constexpr (!Grow) q.reserve(4);
    q.emplace_front(7);
    if constexpr (Grow) {
        // First copy relocates the existing element; second constructs the
        // newly inserted element from the temporary after reserve succeeds.
        Tracked::copies = 0;
        Tracked::fail_copy = 2;
    } else if constexpr (Front) {
        Tracked::throw_constructor = true;
    } else {
        // emplace_back currently constructs an unnecessary temporary. Throw
        // moving it into storage; a fixed direct-emplacement version succeeds.
        Tracked::throw_move = true;
    }
    bool threw = false;
    try {
        if constexpr (Front) q.emplace_front(8);
        else q.emplace_back(8);
    } catch (const std::runtime_error&) { threw = true; }
    Tracked::throw_constructor = false;
    Tracked::throw_move = false;
    Tracked::fail_copy = 0;
    if constexpr (Front && !Grow) CHECK(threw);
    CHECK(q.size() == (threw ? 1u : 2u));
    CHECK(Tracked::live.size() == q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        CHECK(Tracked::live.contains(std::addressof(q[i])));
    }
    if (threw) {
        CHECK(q.front().value == 7);
    } else {
        CHECK(q[Front ? 1 : 0].value == 7);
        CHECK(q[Front ? 0 : 1].value == 8);
    }
    const auto previous_size = q.size();
    q.emplace_back(9);
    CHECK(q.size() == previous_size + 1);
    CHECK(q[q.size() - 1].value == 9);
}

struct alignas(4096) OverAligned { int value = 0; };

void over_aligned_storage() {
    CircularQueue<OverAligned> q;
    for (int i = 0; i < 20; ++i) {
        auto& item = q.emplace_back();
        CHECK(reinterpret_cast<std::uintptr_t>(std::addressof(item)) % alignof(OverAligned) == 0);
        item.value = i;
    }
    q.reserve(65);
    for (std::size_t i = 0; i < q.size(); ++i) {
        CHECK(reinterpret_cast<std::uintptr_t>(std::addressof(q[i])) % alignof(OverAligned) == 0);
        CHECK(q[i].value == static_cast<int>(i));
    }
}

void const_iteration_is_read_only() {
    using Queue = CircularQueue<int>;
    using Iterator = decltype(std::declval<Queue&>().begin());
    using ConstIterator = decltype(std::declval<const Queue&>().begin());
    constexpr bool writable = std::is_assignable_v<decltype(*std::declval<Iterator&>()), int>;
    constexpr bool readonly = !std::is_assignable_v<decltype(*std::declval<ConstIterator&>()), int>;
    CHECK(writable);
    CHECK(readonly);
    Queue q;
    q.push_back(7);
    const Queue& view = q;
    CHECK(*view.begin() == 7);
    static_assert(std::is_convertible_v<Iterator, ConstIterator>);
    static_assert(!std::is_convertible_v<ConstIterator, Iterator>);
    ConstIterator converted = q.begin();
    CHECK(converted == view.begin());
    CHECK(converted == q.begin());
    CHECK(q.begin() == converted);
    CHECK(view.cbegin() == view.begin());
    CHECK(view.cend() == view.end());
    *q.begin() = 9;
    CHECK(*view.begin() == 9);
    CHECK(++converted == view.end());
    q.push_front(8);
    int expected = 8;
    for (const int& value : view) CHECK(value == expected++);
    CHECK(expected == 10);
}

void back_does_not_print() {
    CircularQueue<int> q;
    q.push_back(7);
    std::ostringstream captured;
    auto* previous = std::cout.rdbuf(captured.rdbuf());
    const int result = q.back();
    std::cout.rdbuf(previous);
    CHECK(result == 7);
    CHECK(captured.str().empty());
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
        alarm(60);
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
    failures += !run_test("deterministic stress", deterministic_stress);
    failures += !run_test("randomized stress", randomized_stress);
    failures += !run_test("destruction releases elements", destruction_releases_elements);
    failures += !run_test("copy constructor independence", copying_is_independent<false>);
    failures += !run_test("copy assignment independence", copying_is_independent<true>);
    failures += !run_test("move constructor ownership", moving_preserves_ownership<false>);
    failures += !run_test("move assignment ownership", moving_preserves_ownership<true>);
    failures += !run_test("reserve does not shrink", reserve_does_not_shrink);
    failures += !run_test("reserve zero", reserve_zero_is_safe);
    failures += !run_test("failed reserve preserves objects", failed_reserve_preserves_objects);
    failures += !run_test("failed front insertion", failed_insertion_preserves_state<true, false>);
    failures += !run_test("failed back insertion", failed_insertion_preserves_state<false, false>);
    failures += !run_test("failed front insertion after growth", failed_insertion_preserves_state<true, true>);
    failures += !run_test("failed back insertion after growth", failed_insertion_preserves_state<false, true>);
    failures += !run_test("over-aligned storage", over_aligned_storage);
    failures += !run_test("const iteration", const_iteration_is_read_only);
    failures += !run_test("back does not print", back_does_not_print);
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All CircularQueue tests passed\n";
}
