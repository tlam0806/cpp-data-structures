// Additional behavioral tests. Run ./run_tests.sh from this directory.
#include "LFUCache.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void check(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
template<class F> void throws(F function) {
    bool caught = false;
    try { function(); } catch (const std::runtime_error&) { caught = true; }
    check(caught, "expected injected exception");
}
using Cache = LFUCache<int, int>;

void move_construction() {
    std::unique_ptr<Cache> moved;
    {
        Cache source(2);
        source.put(1, 10); source.put(2, 20);
        check(source.get(1) == 10, "promote source");
        moved = std::make_unique<Cache>(std::move(source));
        // This implementation gives a move-constructed source zero capacity.
        source.put(99, 99);
        check(!source.get(99), "moved-from zero-capacity cache");
        source = Cache(1);
        source.put(7, 70);
        check(source.get(7) == 70, "reassign and reuse moved-from cache");
    }
    moved->put(3, 30);
    check(!moved->get(2), "move lost frequency/eviction order");
    check(moved->get(1) == 10 && moved->get(3) == 30, "move ownership after source destruction");
}

void move_assignment() {
    Cache destination(1);
    destination.put(8, 80);
    {
        Cache source(3);
        source.put(1, 10); source.put(2, 20); source.put(3, 30);
        destination = std::move(source);
        // Swap-based assignment deliberately transfers destination's old state
        // into source. Check that state remains independently usable.
        check(source.get(8) == 80, "source did not receive old destination state");
        source.put(9, 90);
        check(!source.get(8) && source.get(9) == 90, "source capacity after assignment");
    }
    destination.put(4, 40);
    check(!destination.get(1), "destination capacity/order after assignment");
    check(destination.get(2) == 20 && destination.get(3) == 30 && destination.get(4) == 40,
          "destination owns transferred values");
}

void swaps_and_self_move() {
    Cache a(1), b(2);
    a.put(1, 10); b.put(2, 20); b.put(3, 30);
    a.swap(b);
    a.put(4, 40); b.put(5, 50);
    check(!a.get(2) && a.get(3) == 30 && a.get(4) == 40, "swap capacity two");
    check(!b.get(1) && b.get(5) == 50, "swap capacity one");
    Cache* self = &a;
    a = std::move(*self);
    a.swap(*self);
    check(a.get(3) == 30 && a.get(4) == 40, "self move/swap corrupted cache");
    Cache zero(0);
    a.swap(zero);
    a.put(7, 70);
    check(!a.get(7), "swap zero capacity");
    check(zero.get(3) == 30 && zero.get(4) == 40, "swap populated state into empty cache");
}

struct Value {
    inline static int alive = 0;
    inline static bool fail_construct = false, fail_copy = false, fail_assign = false;
    int number;
    explicit Value(int n) : number(n) {
        if (fail_construct) throw std::runtime_error("construction");
        ++alive;
    }
    Value(const Value& other) : number(other.number) {
        if (fail_copy) throw std::runtime_error("copy");
        ++alive;
    }
    Value(Value&& other) noexcept : number(other.number) { ++alive; }
    Value& operator=(int n) {
        if (fail_assign) throw std::runtime_error("assignment");
        number = n; return *this;
    }
    ~Value() { --alive; }
};

void full_cache_failure() {
    check(Value::alive == 0, "initial lifetime count");
    {
        LFUCache<int, Value> cache(2);
        cache.put(1, 10); cache.put(2, 20);
        Value::fail_construct = true;
        throws([&] { cache.put(3, 30); });
        Value::fail_construct = false;
        check(!cache.get(3), "failed insertion left indexed entry");
        // Basic guarantee only: eviction before failure may lose key 1.
        // Do not impose a strong rollback guarantee that the API never promised.
        auto retained = cache.get(2);
        check(retained && retained->number == 20, "unrelated entry was corrupted");
        cache.put(3, 30); cache.put(4, 40);
        check(cache.get(4)->number == 40, "retry after full-cache failure");
    }
    check(Value::alive == 0, "full-cache failure leaked values");
}

void get_copy_failure() {
    {
        LFUCache<int, Value> cache(1);
        cache.put(1, 10);
        Value::fail_copy = true;
        throws([&] { (void)cache.get(1); });
        Value::fail_copy = false;
        check(Value::alive == 1, "failed get copy leaked values");
        check(cache.get(1)->number == 10, "cache unusable after get copy failure");
        cache.put(2, 20);
        check(!cache.get(1) && cache.get(2)->number == 20, "eviction after get copy failure");
    }
    check(Value::alive == 0, "get copy cleanup");
}

void update_assignment_failure() {
    {
        LFUCache<int, Value> cache(2);
        cache.put(1, 10); cache.put(2, 20);
        Value::fail_assign = true;
        throws([&] { cache.put(1, 11); });
        Value::fail_assign = false;
        check(cache.get(1)->number == 10, "throw-before-write changed stored value");
        cache.put(1, 12);
        check(cache.get(1)->number == 12, "retry update failed");
        cache.put(3, 30);
        check(!cache.get(2) && cache.get(3)->number == 30, "eviction after update failure");
    }
    check(Value::alive == 0, "update failure cleanup");
}

void string_keys_and_lifetimes() {
    {
        LFUCache<std::string, Value> cache(64);
        for (int i = 0; i < 1024; ++i) {
            auto key = std::string(128, 'x') + std::to_string(i);
            cache.put(key, i);
            key.assign(256, 'y'); // Cache must own an independent key.
            check(cache.get(std::string(128, 'x') + std::to_string(i))->number == i,
                  "key depends on caller storage");
            check(Value::alive <= 64, "eviction did not destroy value");
        }
        auto moved = std::make_unique<LFUCache<std::string, Value>>(std::move(cache));
        check(moved->get(std::string(128, 'x') + "1023")->number == 1023, "string-key move ownership");
        LFUCache<std::string, Value> other(1);
        other.put("old", -1);
        other = std::move(*moved);
        moved.reset(); // Destroys the old destination's state.
        check(other.get(std::string(128, 'x') + "1023")->number == 1023, "move assignment key references");
    }
    check(Value::alive == 0, "string-key cache leaked values");
}

bool run(const char* name, void (*test)()) {
    std::cout.flush(); std::cerr.flush();
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        alarm(30);
        int result = 0;
        try { test(); }
        catch (const std::exception& e) { std::cerr << name << ": " << e.what() << '\n'; result = 1; }
        std::cout.flush(); std::cerr.flush(); std::_Exit(result);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    const bool pass = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::cout << (pass ? "PASS: " : "FAIL: ") << name << '\n';
    return pass;
}
}

int main() {
    int failed = 0;
    failed += !run("move construction, source destruction and reuse", move_construction);
    failed += !run("populated move assignment and capacities", move_assignment);
    failed += !run("swaps, self move and zero capacity", swaps_and_self_move);
    failed += !run("full-cache insertion failure and recovery", full_cache_failure);
    failed += !run("throwing value copy in get", get_copy_failure);
    failed += !run("throwing update assignment", update_assignment_failure);
    failed += !run("string-key ownership and value lifetimes", string_keys_and_lifetimes);
    return failed ? 1 : 0;
}
