// Run: clang++ -std=c++20 -Wall -Wextra -Wpedantic -Wconversion
//      -fsanitize=address,undefined -g Deque/test.cpp -o /tmp/deque_test
// Add -DDEQUE_TEST_CONST_API to also compile const iteration and copy tests.
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include "Deque.hpp"

#include <deque>
#include <iostream>
#include <random>
#include <stdexcept>
#include <streambuf>
#include <string>

using lam::Deque;
using lam::CACHE_SIZE;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Suppress the diagnostic printed by every emplace_back, without buffering it.
class QuietDeque {
    class Sink : public std::streambuf {
        int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
    } sink;
    std::streambuf* previous = std::cout.rdbuf(&sink);
public:
    ~QuietDeque() { std::cout.rdbuf(previous); }
};

template<std::size_t B>
void verify(Deque<int, B>& actual, const std::deque<int>& expected) {
    check(actual.size() == expected.size(), "size mismatch");
    check(actual.empty() == expected.empty(), "empty mismatch");
    check((actual.begin() == actual.end()) == expected.empty(), "empty iterator range");
    if (!expected.empty()) {
        check(actual.front() == expected.front(), "front mismatch");
        check(actual.back() == expected.back(), "back mismatch");
    }
    auto it = actual.begin();
    for (std::size_t i = 0; i < expected.size(); ++i) {
        check(actual[i] == expected[i], "subscript mismatch");
        check(it != actual.end(), "iterator ended early");
        check(*it == expected[i], "iterator value mismatch");
        check(&++it == &it, "prefix increment must return iterator reference");
    }
    check(it == actual.end(), "iterator did not reach end");
}

template<std::size_t B>
void boundaries() {
    Deque<int, B> actual;
    std::deque<int> expected;
    verify(actual, expected);
    // Exercise both growth directions, partial blocks, and reuse after draining.
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (int i = 0; i < 257; ++i) {
            const int value = cycle * 1000 + i;
            if (cycle % 2 == 0) {
                actual.push_front(value); expected.push_front(value);
            } else {
                actual.push_back(value); expected.push_back(value);
            }
            verify(actual, expected);
        }
        for (int& value : actual) value += 3;
        for (int& value : expected) value += 3;
        verify(actual, expected);
        while (!expected.empty()) {
            if (expected.size() % 2 == 0) {
                actual.pop_front(); expected.pop_front();
            } else {
                actual.pop_back(); expected.pop_back();
            }
            verify(actual, expected);
        }
    }
    // Sliding windows cross block boundaries without increasing the size.
    for (int i = 0; i < 19; ++i) { actual.push_back(i); expected.push_back(i); }
    for (int i = 0; i < 2000; ++i) {
        actual.pop_front(); expected.pop_front();
        actual.push_back(i); expected.push_back(i);
        verify(actual, expected);
    }
    for (int i = 0; i < 2000; ++i) {
        actual.pop_back(); expected.pop_back();
        actual.push_front(i); expected.push_front(i);
        verify(actual, expected);
    }
}

template<std::size_t B>
void randomized() {
    constexpr unsigned seed = 0xD3E02026;
    std::mt19937 rng(seed);
    Deque<int, B> actual;
    std::deque<int> expected;
    for (int step = 0; step < 25000; ++step) {
        const int value = static_cast<int>(rng() % 2000001) - 1000000;
        unsigned operation = static_cast<unsigned>(rng() % 8);
        if (expected.size() > 256) operation = 4 + static_cast<unsigned>(rng() % 2);
        switch (operation) {
        case 0: actual.push_front(value); expected.push_front(value); break;
        case 1: actual.push_back(value); expected.push_back(value); break;
        case 2: {
            auto& inserted = actual.emplace_front(value);
            expected.emplace_front(value);
            check(&inserted == &actual.front(), "emplace_front returned wrong reference");
            break;
        }
        case 3: {
            auto& inserted = actual.emplace_back(value);
            expected.emplace_back(value);
            check(&inserted == &actual.back(), "emplace_back returned wrong reference");
            break;
        }
        case 4: if (!expected.empty()) { actual.pop_front(); expected.pop_front(); } break;
        case 5: if (!expected.empty()) { actual.pop_back(); expected.pop_back(); } break;
        case 6:
            if (!expected.empty()) {
                const auto index = rng() % expected.size();
                actual[index] = value; expected[index] = value;
            }
            break;
        case 7:
            if (!expected.empty()) { actual.front() = value; expected.front() = value; }
            break;
        }
        try { verify(actual, expected); }
        catch (...) {
            std::cerr << "seed=" << seed << ", block_size=" << B << ", step=" << step << '\n';
            throw;
        }
    }
}

void moves_and_swaps() {
    Deque<int, 3> source;
    for (int i = 0; i < 50; ++i) source.push_back(i);
    source.pop_front();
    Deque<int, 3> moved(std::move(source));
    std::deque<int> expected;
    for (int i = 1; i < 50; ++i) expected.push_back(i);
    verify(moved, expected);
    check(source.empty(), "move constructor source should be empty");
    source.push_front(123);
    verify(source, {123});
    source = std::move(moved);
    verify(source, expected);
    check(moved.empty(), "move assignment source should be empty");
    moved.emplace_back(-7);
    source.swap(moved);
    verify(source, {-7}); verify(moved, expected);
    moved.swap(moved);
    verify(moved, expected);
    Deque<int, 3> empty;
    moved.swap(empty);
    verify(moved, {}); verify(empty, expected);
}

void move_only() {
    Deque<std::unique_ptr<int>, 3> actual;
    actual.push_back(std::make_unique<int>(7));
    actual.push_front(std::make_unique<int>(3));
    auto& inserted = actual.emplace_back(std::make_unique<int>(11));
    check(*inserted == 11 && &inserted == &actual.back(), "move-only emplacement");
    check(*actual.front() == 3 && *actual[1] == 7, "move-only order");
    auto owned = std::move(actual[1]);
    check(*owned == 7 && !actual[1], "move out of subscript");
    // Explicit pops isolate this test from the destructor lifetime regression.
    actual.pop_front(); actual.pop_back(); actual.pop_back();
    check(actual.empty(), "move-only drain");
}

struct Tracked {
    static inline int alive = 0;
    int value;
    explicit Tracked(int v) : value(v) { ++alive; }
    Tracked(const Tracked& other) : value(other.value) { ++alive; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++alive; }
    ~Tracked() { --alive; }
};

void lifetimes() {
    const int baseline = Tracked::alive;
    {
        Deque<Tracked, 3> actual;
        for (int i = 0; i < 80; ++i) {
            if (i % 2) actual.emplace_front(i); else actual.emplace_back(i);
        }
        check(Tracked::alive == baseline + 80, "growth changed live object count");
        actual.pop_front(); actual.pop_back();
        check(Tracked::alive == baseline + 78, "pop did not destroy exactly one element");
    }
    check(Tracked::alive == baseline, "destructor did not destroy remaining elements");
}

struct Throwing {
    int value;
    explicit Throwing(int v) : value(v) {
        if (v < 0) throw std::runtime_error("requested constructor failure");
    }
};
void construction_failure() {
    Deque<Throwing, 1> actual;
    actual.emplace_back(42);
    for (int direction = 0; direction < 2; ++direction) {
        bool threw = false;
        try {
            if (direction) actual.emplace_front(-1); else actual.emplace_back(-1);
        } catch (const std::runtime_error&) { threw = true; }
        check(threw, "element construction should throw");
        check(actual.size() == 1 && actual.front().value == 42, "failed insertion changed contents");
    }
    actual.emplace_front(7); actual.emplace_back(9);
    check(actual.size() == 3 && actual[1].value == 42, "reuse after construction failure");
}

#ifdef DEQUE_TEST_CONST_API
void const_and_copy() {
    Deque<int, 3> original;
    for (int i = 0; i < 30; ++i) original.push_front(i);
    const auto& view = original;
    int expected = 29;
    for (auto it = view.cbegin(); it != view.cend(); ++it) check(*it == expected--, "const iteration");
    Deque<int, 3>::const_iterator converted = original.begin();
    check(converted == original.begin(), "mixed iterator equality");
    check(original.begin() == converted, "reverse mixed iterator equality");
    Deque<int, 3> copy(original);
    copy.front() = -1;
    check(original.front() == 29, "copy must own independent elements");
    Deque<int, 3> assigned;
    assigned.push_back(99);
    assigned = original;
    for (std::size_t i = 0; i < original.size(); ++i) check(assigned[i] == original[i], "copy assignment");
    original = original;
    check(original.size() == 30 && original.front() == 29, "self copy assignment");
}
#endif
} // namespace

int main() {
    int failures = 0;
    auto run = [&](const char* name, auto test) {
        try {
            { QuietDeque quiet; test(); }
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    };
    run("boundaries, block size 1", boundaries<1>);
    run("boundaries, block size 3", boundaries<3>);
    run("boundaries, block size 16", boundaries<16>);
    run("randomized, block size 1", randomized<1>);
    run("randomized, block size 3", randomized<3>);
    run("randomized, default block size", randomized<std::max(std::size_t{1}, CACHE_SIZE / alignof(int))>);
    run("moves and swaps", moves_and_swaps);
    run("move-only elements", move_only);
    run("construction failure", construction_failure);
    run("object lifetimes", lifetimes);
#ifdef DEQUE_TEST_CONST_API
    run("const iteration and copying", const_and_copy);
#endif
    std::cout << failures << " test group(s) failed\n";
    return failures == 0 ? 0 : 1;
}
