// From the repository root:
// clang++ -std=c++20 -O1 -g -fsanitize=address,undefined
//   -fno-sanitize-recover=all LFUCache/test.cpp -o /tmp/lfu-tests
// Then run /tmp/lfu-tests. Child-process isolation requires macOS/Linux.
// Add -DLFU_TEST_COPY_ASSIGNMENT for the separate copy-assignment compile
// regression: the current implicitly generated assignment does not compile.
#include "LFUCache.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <sys/wait.h>
#include <unistd.h>

using lam::LFUCache;

namespace {

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "[DETAIL] line " << __LINE__ << ": " << #expression << '\n'; \
        std::abort(); \
    } \
} while (false)

using Cache = LFUCache<int, int>;

void missing_key() {
    Cache cache(2);
    CHECK(!cache.get(99));
    cache.put(1, 10);
    CHECK(!cache.get(99));
}

void promotion_and_lru_tie() {
    Cache cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    CHECK(cache.get(1) == 10);
    CHECK(cache.get(2) == 20); // both frequency 2; key 1 is older
    cache.put(3, 30);
    CHECK(!cache.get(1));
    CHECK(cache.get(2) == 20);
    CHECK(cache.get(3) == 30);
}

void least_frequency_wins_over_recency() {
    Cache cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    CHECK(cache.get(1) == 10);
    CHECK(cache.get(1) == 10); // frequency 3
    CHECK(cache.get(2) == 20); // frequency 2, but more recent
    cache.put(3, 30);
    CHECK(!cache.get(2));
    CHECK(cache.get(1) == 10);
    CHECK(cache.get(3) == 30);
}

void insertion_order_breaks_ties() {
    Cache cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);
    CHECK(!cache.get(1));
    CHECK(cache.get(2) == 20);
    CHECK(cache.get(3) == 30);
}

void updating_increases_frequency() {
    Cache cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(1, 11);
    cache.put(3, 30);
    CHECK(!cache.get(2));
    CHECK(cache.get(1) == 11);
    CHECK(cache.get(3) == 30);
}

void capacity_one() {
    Cache cache(1);
    cache.put(1, 10);
    CHECK(cache.get(1) == 10);
    CHECK(cache.get(1) == 10);
    cache.put(2, 20);
    CHECK(!cache.get(1));
    CHECK(cache.get(2) == 20);
}

void zero_capacity() {
    std::unique_ptr<Cache> cache;
    try {
        cache = std::make_unique<Cache>(0);
    } catch (const std::invalid_argument&) {
        return; // Explicit rejection or retaining no entries are both valid.
    } catch (const std::length_error&) {
        return;
    }
    cache->put(1, 10);
    cache->put(2, 20);
    CHECK(!cache->get(1));
    CHECK(!cache->get(2));
}

template<class C, bool Assign>
void copy_survives_original() {
    // Explicitly disabling copying is an acceptable ownership policy. This
    // branch is discarded so that deleted copy operations do not break builds.
    constexpr bool supported = Assign ? std::is_copy_assignable_v<C>
                                      : std::is_copy_constructible_v<C>;
    if constexpr (supported) {
        std::unique_ptr<C> copy;
        {
            C original(1);
            original.put(1, 10);
            if constexpr (Assign) {
                copy = std::make_unique<C>(2);
                copy->put(99, 99);
                *copy = original;
            } else {
                copy = std::make_unique<C>(original);
            }
        }
        // Eviction uses the key reference in the copied bucket. It must not
        // refer to the destroyed original's map node.
        copy->put(2, 20);
        CHECK(!copy->get(1));
        CHECK(copy->get(2) == 20);
    } else {
        std::cout << "[INFO] copy operation explicitly disabled\n";
    }
}

struct ThrowingValue {
    static inline bool fail = false;
    int value;
    ThrowingValue& operator=(int v) { value = v; return *this; }
    explicit ThrowingValue(int v) : value(v) {
        if (fail) throw std::runtime_error("injected value construction failure");
    }
};

void failed_insertion_rolls_back() {
    LFUCache<int, ThrowingValue> cache(2);
    cache.put(1, 10);
    ThrowingValue::fail = true;
    bool threw = false;
    try { cache.put(2, 20); }
    catch (const std::runtime_error&) { threw = true; }
    ThrowingValue::fail = false;
    CHECK(threw);
    CHECK(!cache.get(2)); // Must not follow default-constructed iterators.
    const auto existing = cache.get(1);
    CHECK(existing && existing->value == 10);
    cache.put(2, 20);
    const auto retried = cache.get(2);
    CHECK(retried && retried->value == 20);
}

// Deliberately simple O(n) eviction oracle, independent of bucket/list logic.
class ReferenceLFU {
    struct Entry { int value; std::size_t frequency; std::size_t touched; };
    std::unordered_map<int, Entry> entries;
    std::size_t capacity;
    std::size_t clock = 0;
public:
    explicit ReferenceLFU(std::size_t cap) : capacity(cap) {}
    std::optional<int> get(int key) {
        auto it = entries.find(key);
        if (it == entries.end()) return std::nullopt;
        ++it->second.frequency;
        it->second.touched = ++clock;
        return it->second.value;
    }
    void put(int key, int value) {
        if (capacity == 0) return;
        auto found = entries.find(key);
        if (found != entries.end()) {
            found->second.value = value;
            ++found->second.frequency;
            found->second.touched = ++clock;
            return;
        }
        if (entries.size() == capacity) {
            auto victim = entries.begin();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (std::tie(it->second.frequency, it->second.touched) <
                    std::tie(victim->second.frequency, victim->second.touched)) victim = it;
            }
            entries.erase(victim);
        }
        entries.emplace(key, Entry{value, 1, ++clock});
    }
};

void randomized_differential() {
    std::mt19937 random(0x1F0CAFEu);
    for (std::size_t capacity : {1u, 2u, 8u}) {
        Cache actual(capacity);
        ReferenceLFU expected(capacity);
        for (int step = 0; step < 10'000; ++step) {
            const int key = static_cast<int>(random() % 16);
            if (random() % 2 == 0) {
                const int value = static_cast<int>(random() % 1000);
                actual.put(key, value);
                expected.put(key, value);
            } else {
                CHECK(actual.get(key) == expected.get(key));
            }
        }
        // Queries update frequencies in both implementations identically.
        for (int key = 0; key < 16; ++key) CHECK(actual.get(key) == expected.get(key));
    }
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
        alarm(30); // A corrupt list can loop rather than crash.
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
    failures += !run_test("missing key", missing_key);
    failures += !run_test("promotion and LRU tie", promotion_and_lru_tie);
    failures += !run_test("LFU before recency", least_frequency_wins_over_recency);
    failures += !run_test("insertion order tie", insertion_order_breaks_ties);
    failures += !run_test("updating increases frequency", updating_increases_frequency);
    failures += !run_test("capacity one", capacity_one);
    failures += !run_test("zero capacity", zero_capacity);
    failures += !run_test("copy constructor ownership", copy_survives_original<Cache, false>);
#ifdef LFU_TEST_COPY_ASSIGNMENT
    failures += !run_test("copy assignment ownership", copy_survives_original<Cache, true>);
#else
    std::cout << "[SKIP] copy assignment: compile with -DLFU_TEST_COPY_ASSIGNMENT to check\n";
#endif
    failures += !run_test("failed insertion rollback", failed_insertion_rolls_back);
    failures += !run_test("randomized differential", randomized_differential);
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All LFU cache tests passed\n";
}
