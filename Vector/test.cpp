#include "vector.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using lam::Vector;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class F>
void expect_throw(F&& operation) {
    bool caught = false;
    try { operation(); }
    catch (const Exception&) { caught = true; }
    check(caught, "expected exception was not thrown");
}

template<class T>
void same(const Vector<T>& actual, const std::vector<T>& expected) {
    check(actual.size() == expected.size(), "size mismatch");
    check(actual.empty() == expected.empty(), "empty mismatch");
    check(actual.capacity() >= actual.size(), "invalid capacity");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        check(actual[i] == expected[i], "element mismatch");
        check(actual.at(i) == expected.at(i), "at mismatch");
    }
    if (!expected.empty()) {
        check(actual.front() == expected.front(), "front mismatch");
        check(actual.back() == expected.back(), "back mismatch");
        check(actual.end() - actual.begin() == static_cast<std::ptrdiff_t>(actual.size()),
              "iterator distance mismatch");
    } else {
        check(actual.begin() == actual.end(), "empty iterator range");
    }
}

// A throwing move forces reserve/shrink_to_fit to copy when possible.
// Count only successfully constructed objects, including moved-from objects.
struct Tracked {
    inline static int alive = 0;
    inline static int copies_before_throw = -1;
    inline static int defaults_before_throw = -1;
    inline static int assignments_before_throw = -1;
    int value;

    static void checkpoint(int& remaining) {
        if (remaining == 0) throw std::runtime_error("injected failure");
        if (remaining > 0) --remaining;
    }
    Tracked() : value(0) { checkpoint(defaults_before_throw); ++alive; }
    explicit Tracked(int v) : value(v) { ++alive; }
    Tracked(const Tracked& other) : value(other.value) {
        checkpoint(copies_before_throw); ++alive;
    }
    Tracked(Tracked&& other) noexcept(false) : value(other.value) {
        other.value = -1; ++alive;
    }
    Tracked& operator=(const Tracked& other) {
        checkpoint(assignments_before_throw); value = other.value; return *this;
    }
    Tracked& operator=(Tracked&& other) noexcept(false) {
        checkpoint(assignments_before_throw);
        value = other.value; other.value = -1; return *this;
    }
    ~Tracked() { --alive; }
    static void reset_failures() {
        copies_before_throw = defaults_before_throw = assignments_before_throw = -1;
    }
};

void basics() {
    Vector<int> a;
    same(a, {});
    expect_throw<std::out_of_range>([&] { (void)a.at(0); });
    check(a.erase(a.begin(), a.end()) == a.end(), "empty range erase");
    a.insert(a.end(), 7);
    a.emplace_back(9);
    a.insert(a.begin(), 3);
    same(a, {3, 7, 9});
    const auto& c = a;
    static_assert(std::is_same_v<decltype(c.data()), const int*>);
    static_assert(std::is_same_v<decltype(c.begin()), const int*>);
    static_assert(std::is_same_v<decltype(c.at(0)), const int&>);
    expect_throw<std::out_of_range>([&] { (void)c.at(c.size()); });
    for (int& value : a) value *= 2;
    same(a, {6, 14, 18});
    auto capacity = a.capacity();
    auto data = a.data();
    a.reserve(capacity);
    check(a.data() == data, "reserve within capacity relocated elements");
    a.clear();
    check(a.empty() && a.capacity() == capacity, "clear capacity");
    a.shrink_to_fit();
    same(a, {});
    a.push_back(42);
    same(a, {42});
    same(Vector<int>(4), {0, 0, 0, 0});
    same(Vector<int>(3, 8), {8, 8, 8});
}

void ownership() {
    Vector<std::string> original{"one", "two"};
    Vector<std::string> copy(original);
    copy[0] = "changed";
    check(original[0] == "one", "copy shares storage");
    Vector<std::string> assigned;
    assigned = original;
    auto* self = &assigned;
    assigned = *self;
    same(assigned, {"one", "two"});
    auto* storage = original.data();
    Vector<std::string> moved(std::move(original));
    check(moved.data() == storage, "move construction did not transfer storage");
    original.emplace_back("reused");
    assigned = std::move(moved);
    same(assigned, {"one", "two"});
    moved.emplace_back("also reused");
    assigned = std::move(*self);
    check(assigned.size() <= assigned.capacity(), "self move invalid state");
    assigned.clear();
    assigned.emplace_back("valid");
    original.swap(moved);
    same(original, {"also reused"});
    same(moved, {"reused"});
}

void aliases() {
    Vector<std::string> a{"alpha", "beta"};
    a.shrink_to_fit();
    a.push_back(a.front()); // Full capacity: argument must survive reallocation.
    same(a, {"alpha", "beta", "alpha"});
    a.shrink_to_fit();
    a.insert(a.begin() + 1, a.back());
    same(a, {"alpha", "alpha", "beta", "alpha"});
    a.reserve(20);
    a.insert(a.begin(), a[2]); // Argument also aliases a shifted element.
    same(a, {"beta", "alpha", "alpha", "beta", "alpha"});
    a.shrink_to_fit();
    a.resize(8, a[1]);
    same(a, {"beta", "alpha", "alpha", "beta", "alpha", "alpha", "alpha", "alpha"});
}

void move_only() {
    Vector<std::unique_ptr<int>> a;
    a.push_back(std::make_unique<int>(1));
    a.emplace_back(std::make_unique<int>(3));
    a.insert(a.begin() + 1, std::make_unique<int>(2));
    a.reserve(32);
    a.shrink_to_fit();
    check(a.size() == 3 && *a[0] == 1 && *a[1] == 2 && *a[2] == 3, "move-only values");
    a.erase(a.begin());
    a.resize(4);
    check(*a[0] == 2 && *a[1] == 3 && !a[2] && !a[3], "move-only resize/erase");
    a.clear();
}

void randomized() {
    // Compare observable behavior, not implementation-specific growth policies.
    for (unsigned seed : {1u, 17u, 20260910u}) {
        std::mt19937 rng(seed);
        Vector<int> a;
        std::vector<int> b;
        for (int step = 0; step < 10000; ++step) {
            const int value = static_cast<int>(rng() % 1000);
            switch (rng() % 9) {
            case 0: a.push_back(value); b.push_back(value); break;
            case 1: if (!b.empty()) { a.pop_back(); b.pop_back(); } break;
            case 2: {
                auto n = static_cast<std::size_t>(rng() % 128);
                a.resize(n, value); b.resize(n, value); break;
            }
            case 3: {
                auto index = static_cast<std::size_t>(rng()) % (b.size() + 1);
                auto pos = a.empty() ? a.end() : a.begin() + index;
                auto result = a.insert(pos, value);
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(index), value);
                check(result == a.begin() + index && *result == value, "insert return");
                break;
            }
            case 4: if (!b.empty()) {
                auto index = static_cast<std::size_t>(rng()) % b.size();
                auto result = a.erase(a.begin() + index);
                b.erase(b.begin() + static_cast<std::ptrdiff_t>(index));
                check(result == a.begin() + index, "erase return");
            } break;
            case 5: if (!b.empty()) {
                auto first = static_cast<std::size_t>(rng()) % (b.size() + 1);
                auto last = first + static_cast<std::size_t>(rng()) % (b.size() - first + 1);
                auto result = a.erase(a.begin() + first, a.begin() + last);
                b.erase(b.begin() + static_cast<std::ptrdiff_t>(first), b.begin() + static_cast<std::ptrdiff_t>(last));
                check(result == a.begin() + first, "range erase return");
            } break;
            case 6: a.reserve(static_cast<std::size_t>(rng() % 256)); break;
            case 7: a.shrink_to_fit(); break;
            case 8: a.clear(); b.clear(); break;
            }
            same(a, b);
        }
    }
}

void exceptions() {
    check(Tracked::alive == 0, "initial lifetime count");
    {
        Vector<Tracked> a;
        a.reserve(3);
        for (int i = 0; i < 3; ++i) a.emplace_back(i + 1);
        auto* original = a.data();
        Tracked::copies_before_throw = 1;
        expect_throw<std::runtime_error>([&] { a.reserve(20); });
        check(a.data() == original && a.size() == 3 && Tracked::alive == 3, "reserve rollback");
        for (std::size_t i = 0; i < 3; ++i) check(a[i].value == static_cast<int>(i + 1), "reserve changed values");
        Tracked::copies_before_throw = 1;
        expect_throw<std::runtime_error>([&] { Vector<Tracked> copy(a); });
        check(Tracked::alive == 3, "failed copy construction leaked objects");
        Tracked::reset_failures();
        Vector<Tracked> destination;
        destination.emplace_back(99);
        Tracked::copies_before_throw = 1;
        expect_throw<std::runtime_error>([&] { destination = a; });
        check(destination.size() == 1 && destination[0].value == 99 && Tracked::alive == 4, "copy assignment rollback");
        Tracked::reset_failures();
        a.reserve(20);
        Tracked::copies_before_throw = 1;
        expect_throw<std::runtime_error>([&] { a.shrink_to_fit(); });
        check(a.size() == 3 && a.capacity() == 20 && Tracked::alive == 4, "shrink rollback");
        Tracked::reset_failures();
        Tracked::defaults_before_throw = 1;
        expect_throw<std::runtime_error>([&] { a.resize(6); });
        check(a.size() == 3 && Tracked::alive == 4, "resize default rollback");
        Tracked::reset_failures();
        Tracked::copies_before_throw = 2; // Temporary + one new element succeed.
        expect_throw<std::runtime_error>([&] { a.resize(6, a.front()); });
        check(a.size() == 3 && Tracked::alive == 4, "resize fill rollback");
        Tracked::reset_failures();
        Tracked::defaults_before_throw = 0;
        expect_throw<std::runtime_error>([&] { a.emplace_back(); });
        check(a.size() == 3 && Tracked::alive == 4, "emplace failure changed size");
        Tracked::reset_failures();
        a.resize(1);
        check(Tracked::alive == 2, "resize shrink did not destroy elements");
        a.clear();
        check(Tracked::alive == 1, "clear did not destroy elements");
    }
    check(Tracked::alive == 0, "destructor leaked elements");
    Tracked::defaults_before_throw = 2;
    expect_throw<std::runtime_error>([] { Vector<Tracked> a(5); });
    Tracked::reset_failures();
    check(Tracked::alive == 0, "failed count constructor leaked elements");
    {
        Tracked value(7);
        Tracked::copies_before_throw = 2;
        expect_throw<std::runtime_error>([&] { Vector<Tracked> a(5, value); });
        Tracked::reset_failures();
        check(Tracked::alive == 1, "failed fill constructor leaked elements");
    }
    Tracked::copies_before_throw = 1;
    expect_throw<std::runtime_error>([] { Vector<Tracked> a{Tracked(1), Tracked(2), Tracked(3)}; });
    Tracked::reset_failures();
    check(Tracked::alive == 0, "failed initializer list constructor leaked elements");
    {
        Vector<Tracked> a;
        a.reserve(1);
        a.emplace_back(1);
        // Existing element is copied successfully; appending the temporary fails.
        Tracked::copies_before_throw = 1;
        expect_throw<std::runtime_error>([&] { a.emplace_back(2); });
        Tracked::reset_failures();
        check(a.size() == 1 && a[0].value == 1 && Tracked::alive == 1,
              "failed append after growth leaked or changed an element");
    }
    check(Tracked::alive == 0, "failed append cleanup");
}

void throwing_insert() {
    // Middle insertion promises valid, destructible storage if assignment throws,
    // not unchanged values. Cover both overloads and each assignment position.
    for (bool rvalue : {false, true}) {
        for (int failure : {0, 1, 2}) {
            {
                Vector<Tracked> a;
                a.reserve(8);
                for (int i = 0; i < 3; ++i) a.emplace_back(i);
                Tracked value(9);
                Tracked::assignments_before_throw = failure;
                expect_throw<std::runtime_error>([&] {
                    if (rvalue) a.insert(a.begin(), std::move(value));
                    else a.insert(a.begin(), value);
                });
                Tracked::reset_failures();
                check(Tracked::alive == static_cast<int>(a.size()) + 1,
                      "throwing insert leaves an untracked live element");
                a.clear();
                a.emplace_back(42);
                check(a[0].value == 42, "reuse after failed insertion");
            }
            check(Tracked::alive == 0, "throwing insert leaked elements");
        }
    }
}

struct alignas(256) Aligned { int value = 0; };
void alignment_and_limits() {
    Vector<Aligned> a(3);
    a.reserve(32);
    a.shrink_to_fit();
    Vector<Aligned> copy(a);
    for (auto* vector : {&a, &copy}) {
        for (const auto& element : *vector) {
            check(reinterpret_cast<std::uintptr_t>(&element) % alignof(Aligned) == 0,
                  "over-aligned element has misaligned storage");
        }
    }
    a.clear();
    a.shrink_to_fit();
    expect_throw<std::length_error>([&] { a.reserve(a.max_size() + 1); });
    check(a.empty(), "oversized reserve changed vector");
}
} // namespace

int main() {
    int failed = 0;
    const std::pair<const char*, void(*)()> tests[] = {
        {"basic operations and const access", basics},
        {"copy, move, swap and reuse", ownership},
        {"self-referencing arguments", aliases},
        {"move-only elements", move_only},
        {"30,000 differential operations", randomized},
        {"exceptions and object lifetimes", exceptions},
        {"throwing middle insertion", throwing_insert},
        {"over-alignment and capacity limits", alignment_and_limits},
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << std::endl; }
        catch (const std::exception& e) {
            ++failed;
            Tracked::reset_failures();
            std::cerr << "FAIL: " << name << ": " << e.what() << std::endl;
        }
    }
    return failed == 0 ? 0 : 1;
}
