// Multithreaded stress tests for the atomic reference counts.
// Build from this directory with ThreadSanitizer (cannot be combined with ASan):
// clang++ -std=c++20 -g -O1 -fsanitize=thread thread_test.cpp -o thread_test && ./thread_test
// Optional: ./thread_test --list or ./thread_test lock_vs_last_release
//
// TSan reports missing synchronization (e.g. a decrement that is too weak) as
// "WARNING: ThreadSanitizer: data race". The CHECKs below catch logic races that
// TSan can't see, such as an object destroyed twice or revived by lock().
#include "shared_ptr.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <string_view>
#include <thread>
#include <vector>

// Counts live heap allocations, to check control blocks are freed exactly once.
static std::atomic<long> live_allocations{0};

void* operator new(std::size_t size) {
    if (void* p = std::malloc(size ? size : 1)) {
        live_allocations.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if (p) live_allocations.fetch_sub(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

namespace {
const char* current_test = "";

void check(bool condition, const char* expression,
           std::source_location where = std::source_location::current()) {
    if (!condition) {
        std::cerr << "[CHECK FAILED] " << current_test << '\n'
                  << "  " << where.file_name() << ':' << where.line() << '\n'
                  << "  Expected true: " << expression << '\n';
        std::cerr.flush();
        std::_Exit(1);
    }
}
#define CHECK(expression) check((expression), #expression)

constexpr int kThreads = 4;

struct Payload {
    static inline std::atomic<int> destroyed{0};
    static inline std::atomic<int> incomplete_destructor_reads{0};
    int magic = 42;
    int slots[kThreads]{};
    int expected_writes = 0;
    ~Payload() {
        // Plain reads of fields other threads wrote before releasing their copies.
        int sum = 0;
        for (int s : slots) sum += s;
        if (sum != expected_writes) incomplete_destructor_reads.fetch_add(1);
        magic = 0;
        destroyed.fetch_add(1);
    }
};

// Alternate both control block kinds: separate allocation and make_shared's inline block.
shared_ptr<Payload> make_payload(int iteration) {
    if (iteration % 2 == 0) return shared_ptr<Payload>(new Payload);
    return ::make_shared<Payload>();
}

void spin_until(const std::atomic<bool>& go) {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
}

// ---------------------------------------------------------------- tests

// Many threads copy and destroy copies of one shared_ptr. Lost updates would leave
// the count wrong, destroying the object early or never.
void concurrent_copies() {
    long before = live_allocations.load();
    {
        auto p = ::make_shared<Payload>();
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                spin_until(go);
                for (int i = 0; i < 20000; ++i) {
                    shared_ptr<Payload> copy(p);
                    shared_ptr<Payload> moved(std::move(copy));
                    CHECK(moved->magic == 42);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        CHECK(p.use_count() == 1);
        CHECK(Payload::destroyed.load() == 0);
    }
    CHECK(Payload::destroyed.load() == 1);
    CHECK(live_allocations.load() == before);
}

// Each thread writes to the object, then drops its copy; whichever thread is last
// runs the destructor, which must see every write (needs release + acquire).
void last_owner_sees_all_writes() {
    constexpr int iterations = 500;
    long before = live_allocations.load();
    for (int iter = 0; iter < iterations; ++iter) {
        auto p = make_payload(iter);
        p->expected_writes = kThreads;
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&go, t, mine = shared_ptr<Payload>(p)]() mutable {
                spin_until(go);
                mine->slots[t] = 1;
                mine.reset();
            });
        }
        p.reset();
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
    }
    CHECK(Payload::destroyed.load() == iterations);
    CHECK(Payload::incomplete_destructor_reads.load() == 0);
    CHECK(live_allocations.load() == before);
}

// One thread drops the last owner while others call lock(). lock() must either
// fail or return a live object, never revive a dead one.
void lock_vs_last_release() {
    constexpr int iterations = 500;
    long before = live_allocations.load();
    std::atomic<int> dead_objects_seen{0};
    std::atomic<long> successful_locks{0};
    for (int iter = 0; iter < iterations; ++iter) {
        auto p = make_payload(iter);
        weak_ptr<Payload> w(p);
        std::atomic<bool> go{false};
        std::thread releaser([&] {
            spin_until(go);
            p.reset();
        });
        std::vector<std::thread> lockers;
        for (int t = 0; t < 3; ++t) {
            lockers.emplace_back([&] {
                spin_until(go);
                for (int i = 0; i < 100; ++i) {
                    shared_ptr<Payload> s = w.lock();
                    if (s) {
                        successful_locks.fetch_add(1, std::memory_order_relaxed);
                        if (s->magic != 42) dead_objects_seen.fetch_add(1);
                    }
                }
            });
        }
        go.store(true, std::memory_order_release);
        releaser.join();
        for (auto& t : lockers) t.join();
        CHECK(w.expired());
        CHECK(!w.lock());
    }
    CHECK(dead_objects_seen.load() == 0);
    CHECK(Payload::destroyed.load() == iterations);
    CHECK(live_allocations.load() == before);
    std::cout << "  (" << successful_locks.load() << " successful locks raced the release)\n";
}

// The last shared_ptr and the last weak_ptr die at the same moment. Exactly one
// of them must free the control block.
void last_shared_and_weak_die_together() {
    constexpr int iterations = 1000;
    long before = live_allocations.load();
    for (int iter = 0; iter < iterations; ++iter) {
        auto p = make_payload(iter);
        weak_ptr<Payload> w(p);
        std::atomic<bool> go{false};
        std::thread a([&] { spin_until(go); p.reset(); });
        std::thread b([&] { spin_until(go); w.reset(); });
        go.store(true, std::memory_order_release);
        a.join();
        b.join();
    }
    CHECK(Payload::destroyed.load() == iterations);
    CHECK(live_allocations.load() == before);
}

// Many threads copy, lock and destroy weak_ptrs while the object stays alive.
void concurrent_weak_copies_and_locks() {
    long before = live_allocations.load();
    {
        auto p = ::make_shared<Payload>();
        weak_ptr<Payload> w(p);
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                spin_until(go);
                for (int i = 0; i < 20000; ++i) {
                    weak_ptr<Payload> copy(w);
                    shared_ptr<Payload> s = copy.lock();
                    CHECK(static_cast<bool>(s));
                    CHECK(s->magic == 42);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        CHECK(p.use_count() == 1);
        CHECK(!w.expired());
    }
    CHECK(Payload::destroyed.load() == 1);
    CHECK(live_allocations.load() == before);
}

struct Test { const char* name; void (*run)(); };
} // namespace

int main(int argc, char** argv) {
    const Test tests[] = {
        {"concurrent_copies", concurrent_copies},
        {"last_owner_sees_all_writes", last_owner_sees_all_writes},
        {"lock_vs_last_release", lock_vs_last_release},
        {"last_shared_and_weak_die_together", last_shared_and_weak_die_together},
        {"concurrent_weak_copies_and_locks", concurrent_weak_copies_and_locks},
    };
    if (argc == 2 && std::string_view(argv[1]) == "--list") {
        for (const auto& t : tests) std::cout << t.name << '\n';
        return 0;
    }
    int count = 0;
    for (const auto& t : tests) {
        if (argc == 2 && std::string_view(argv[1]) != t.name) continue;
        ++count;
        current_test = t.name;
        Payload::destroyed = 0;
        Payload::incomplete_destructor_reads = 0;
        std::cout << "[RUN] " << t.name << std::endl;
        t.run();
        std::cout << "[PASS] " << t.name << std::endl;
    }
    if (!count) { std::cerr << "Unknown test; use --list\n"; return 2; }
    return 0;
}
