// Build from the repository root:
// clang++ -std=c++20 -O1 -g -fsanitize=address,undefined
//   -fno-sanitize-recover=all HashMap/test.cpp -o /tmp/hashmap-tests
// Run all: /tmp/hashmap-tests
// List tests: /tmp/hashmap-tests --list
// Run one: /tmp/hashmap-tests erase_by_key
// Child-process isolation and shared crash context require macOS/Linux.
// Duplicate insert intentionally updates values, matching this project's API.
#include "HashMap.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <random>
#include <type_traits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <new>
#include <source_location>
#include <sstream>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using lam::HashMap;

namespace {

struct CrashContext {
    char actions[8][384]{};
    std::size_t count = 0;
};
CrashContext* context = nullptr;
const char* current_test = "";

void trace(std::string_view action, const std::source_location where = std::source_location::current()) {
    auto& slot = context->actions[context->count % 8];
    std::snprintf(slot, sizeof(slot), "line %u: %.*s", where.line(),
                  static_cast<int>(action.size()), action.data());
    ++context->count;
}

void print_recent_actions() {
    std::cerr << "  Recent actions (last may have crashed):\n";
    const auto first = context->count > 8 ? context->count - 8 : 0;
    for (auto i = first; i < context->count; ++i) {
        std::cerr << "    " << context->actions[i % 8] << '\n';
    }
}

template<class T>
std::string show(const T& value) {
    std::ostringstream out;
    out << std::boolalpha;
    if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        out << std::quoted(std::string(std::string_view(value)));
    } else if constexpr (requires { out << value; }) {
        out << value;
    } else {
        out << "<unprintable>";
    }
    return out.str();
}

[[noreturn]] void report_failure(const char* operation, std::size_t step, int key,
                                std::string_view actual, std::string_view expected,
                                const std::source_location& where) {
    std::cerr << "\n[CHECK FAILED] " << current_test << '\n'
              << "  " << where.file_name() << ':' << where.line() << '\n'
              << "  Check: " << operation << '\n'
              << "  Step: " << step << ", key: " << key << " (0 when unspecified)\n"
              << "  Expected: " << expected << '\n'
              << "  Actual:   " << actual << '\n';
    // Exit immediately: do not traverse a potentially corrupt map to print it,
    // or let its destructor mask the original failure with another crash.
    std::cerr.flush();
    std::_Exit(1);
}

void check(bool condition, const char* operation, std::size_t step = 0, int key = 0,
           const std::source_location where = std::source_location::current()) {
    if (!condition) report_failure(operation, step, key, "false", "true", where);
}

template<class Actual, class Expected>
void check_equal(const Actual& actual, const Expected& expected, const char* operation,
                 std::size_t step = 0, int key = 0,
                 const std::source_location where = std::source_location::current()) {
    if (!(actual == expected)) {
        report_failure(operation, step, key, show(actual), show(expected), where);
    }
}

struct ConstantHash {
    std::size_t operator()(int) const noexcept {
        return 0;
    }
};

struct CountingHash {
    static inline std::size_t calls = 0;

    std::size_t operator()(int key) const noexcept {
        ++calls;
        return std::hash<int>{}(key);
    }
};

struct Tracked {
    static inline int alive = 0;

    int value = 0;

    Tracked() {
        ++alive;
    }

    explicit Tracked(int value_) : value{value_} {
        ++alive;
    }

    Tracked(const Tracked& other) : value{other.value} {
        ++alive;
    }

    Tracked(Tracked&& other) noexcept : value{other.value} {
        ++alive;
    }

    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;

    ~Tracked() {
        --alive;
    }
};

template<typename Hash>
void check_against_reference(
    HashMap<int, int, Hash>& actual,
    const std::unordered_map<int, int>& expected,
    std::size_t step)
{
    check_equal(actual.size(), expected.size(), "size mismatch", step);
    check_equal(actual.empty(), expected.empty(), "empty mismatch", step);

    for (const auto& [key, value] : expected) {
        trace("auto found = actual.find(key);");
        auto found = actual.find(key);
        check(found != actual.end(), "missing key", step, key);
        check_equal(found->first, key, "find returned wrong key", step, key);
        check_equal(found->second, value, "wrong mapped value", step, key);
    }

    std::unordered_set<int> visited;
    std::size_t count = 0;

    for (auto it = actual.begin(); it != actual.end(); ++it) {
        ++count;
        check(count <= expected.size(), "iterator cycle or extra element", step);

        const auto reference_it = expected.find(it->first);
        check(reference_it != expected.end(), "iteration returned unknown key", step, it->first);
        check_equal(reference_it->second, it->second, "iteration returned wrong value", step, it->first);
        check(visited.insert(it->first).second, "iteration returned duplicate key", step, it->first);
    }

    check_equal(count, expected.size(), "iteration ended early", step);
}

void test_empty_map() {
    HashMap<int, int> map;

    check(map.empty(), "new map not empty");
    check_equal(map.size(), 0, "new map size");
    check_equal(map.begin(), map.end(), "empty begin/end");
    check_equal(map.find(123), map.end(), "find in empty map", 0, 123);
}

void test_insert_find_and_subscript() {
    HashMap<int, std::string> map;

    trace("auto first = map.insert({1, \"one\"});");
    auto first = map.insert({1, "one"});
    check(first != map.end(), "insert returned end", 0, 1);
    check_equal(first->first, 1, "rvalue insert key", 0, 1);
    check_equal(first->second, "one", "rvalue insert value", 0, 1);
    check_equal(map.size(), 1, "size after rvalue insert");

    std::pair<const int, std::string> second{2, "two"};
    trace("auto second_it = map.insert(second);");
    auto second_it = map.insert(second);
    check(second_it != map.end(), "insert returned end", 0, 2);
    check_equal(second_it->second, "two", "lvalue insert", 0, 2);
    check_equal(map.size(), 2, "size after lvalue insert");

    trace("map.insert({1, \"ONE\"});");
    map.insert({1, "ONE"});
    check_equal(map.size(), 2, "duplicate insert changed size", 0, 1);
    check_equal(map.find(1)->second, "ONE", "duplicate insert did not update", 0, 1);

    int lvalue_key = 3;
    check(map[lvalue_key].empty(), "lvalue subscript not default initialized", 0, lvalue_key);
    trace("map[lvalue_key] = \"three\";");
    map[lvalue_key] = "three";
    check_equal(map[lvalue_key], "three", "lvalue subscript assignment", 0, lvalue_key);

    trace("map[4] = \"four\";");
    map[4] = "four";
    check_equal(map[4], "four", "rvalue subscript assignment", 0, 4);
    check_equal(map.size(), 4, "subscript size");
}

void test_collisions_and_iteration() {
    HashMap<int, int, ConstantHash> map;
    std::unordered_map<int, int> expected;

    for (int key = 0; key < 200; ++key) {
        trace("map.insert({key, key * 11}); key=" + std::to_string(key));
        map.insert({key, key * 11});
        expected.insert_or_assign(key, key * 11);
    }

    check_against_reference(map, expected, 200);

    for (int key = 0; key < 200; key += 3) {
        trace("map[key] = -key; key=" + std::to_string(key));
        map[key] = -key;
        expected[key] = -key;
    }

    check_against_reference(map, expected, 201);
}

void test_automatic_rehash() {
    constexpr int element_count = 2'000;

    CountingHash::calls = 0;
    HashMap<int, int, CountingHash> map;
    std::unordered_map<int, int> expected;
    std::vector<std::pair<int, int*>> saved_addresses;
    bool observed_redistribution = false;

    for (int key = 0; key < element_count; ++key) {
        const std::size_t calls_before = CountingHash::calls;
        trace("auto inserted = map.insert({key, key * 17}); key=" + std::to_string(key));
        auto inserted = map.insert({key, key * 17});
        const std::size_t calls_for_insert = CountingHash::calls - calls_before;

        if (calls_for_insert > 1) {
            observed_redistribution = true;
        }

        expected.insert_or_assign(key, key * 17);
        check_equal(inserted->first, key, "rehash insertion returned wrong key", static_cast<std::size_t>(key), key);
        check_equal(inserted->second, key * 17, "rehash insertion returned wrong value", static_cast<std::size_t>(key), key);

        if (key < 12) {
            saved_addresses.push_back({key, &inserted->second});
        }
    }

    check(observed_redistribution, "automatic rehash was not observed");
    check_against_reference(map, expected, element_count);

    for (const auto& [key, old_address] : saved_addresses) {
        trace("auto found = map.find(key);");
        auto found = map.find(key);
        check(found != map.end(), "rehash lost referenced key", element_count, key);
        check_equal(&found->second, old_address, "rehash changed element address", element_count, key);
        check_equal(*old_address, key * 17, "rehash invalidated element reference", element_count, key);
    }

    trace("const std::size_t size_before_duplicate = map.size();");
    const std::size_t size_before_duplicate = map.size();
    const std::size_t calls_before_duplicate = CountingHash::calls;
    trace("map.insert({17, -17});");
    map.insert({17, -17});
    const std::size_t duplicate_hash_calls = CountingHash::calls - calls_before_duplicate;

    check_equal(map.size(), size_before_duplicate, "duplicate insertion changed size after rehash", 0, 17);
    check_equal(map.find(17)->second, -17, "duplicate insertion value after rehash", 0, 17);
    check_equal(duplicate_hash_calls, 1, "duplicate insertion triggered rehash", 0, 17);
}

void test_explicit_rehash() {
    HashMap<int, int> map;
    std::unordered_map<int, int> expected;

    for (int key = 0; key < 80; ++key) {
        trace("map.insert({key, key + 1'000}); key=" + std::to_string(key));
        map.insert({key, key + 1'000});
        expected.insert_or_assign(key, key + 1'000);
    }

    trace("int* saved_address = &map.find(7)->second;");
    int* saved_address = &map.find(7)->second;
    trace("map.rehash(257);");
    map.rehash(257);

    check_equal(map.size(), expected.size(), "explicit rehash changed size");
    check_against_reference(map, expected, 257);
    check_equal(&map.find(7)->second, saved_address, "explicit rehash changed element address", 0, 7);
    check_equal(*saved_address, 1'007, "explicit rehash invalidated reference", 0, 7);

    trace("map.rehash(257);");
    map.rehash(257);
    check_against_reference(map, expected, 258);

    HashMap<int, int> empty;
    trace("empty.rehash(32);");
    empty.rehash(32);
    check(empty.empty(), "explicit rehash changed empty map");
    check(empty.begin() == empty.end(), "explicit rehash broke empty iteration");
}

void test_erase_by_key() {
    HashMap<int, int, ConstantHash> map;
    trace("map.insert({1, 10});");
    map.insert({1, 10});
    trace("map.insert({2, 20});");
    map.insert({2, 20});
    trace("map.insert({3, 30});");
    map.insert({3, 30});

    check(map.erase(3) == 1, "erase bucket head", 0, 3);
    check(map.find(3) == map.end(), "erased head still found", 0, 3);
    check(map.size() == 2, "size after erasing head", 0, 3);

    check(map.erase(1) == 1, "erase bucket tail", 0, 1);
    check(map.find(1) == map.end(), "erased tail still found", 0, 1);

    check(map.erase(2) == 1, "erase only node", 0, 2);
    check(map.empty(), "map not empty after erasing all");
    check(map.erase(999) == 0, "erase missing key", 0, 999);
}

void test_erase_by_iterator() {
    HashMap<int, int, ConstantHash> map;
    trace("map.insert({1, 10});");
    map.insert({1, 10});
    trace("map.insert({2, 20});");
    map.insert({2, 20});
    trace("map.insert({3, 30});");
    map.insert({3, 30});
    trace("map.insert({4, 40});");
    map.insert({4, 40});

    trace("auto victim = map.find(2);");
    auto victim = map.find(2);
    check(victim != map.end(), "erase target missing", 0, 2);
    auto expected_next = victim;
    ++expected_next;

    trace("const bool should_return_end = expected_next == map.end();");
    const bool should_return_end = expected_next == map.end();
    const int expected_next_key = should_return_end ? 0 : expected_next->first;
    trace("auto returned = map.erase(victim);");
    auto returned = map.erase(victim);

    check(map.find(2) == map.end(), "iterator erase left key", 0, 2);
    check(map.size() == 3, "size after iterator erase", 0, 2);
    check((returned == map.end()) == should_return_end, "iterator erase returned wrong position", 0, 2);
    if (!should_return_end) {
        check(returned->first == expected_next_key, "iterator erase returned wrong key", 0, 2);
    }
}

void test_copy_operations() {
    HashMap<int, std::string> original;
    trace("original.insert({1, \"one\"});");
    original.insert({1, "one"});
    trace("original.insert({2, \"two\"});");
    original.insert({2, "two"});
    trace("original.insert({3, \"three\"});");
    original.insert({3, "three"});

    HashMap<int, std::string> copied{original};
    check(copied.size() == original.size(), "copy constructor size");
    trace("copied.insert({2, \"changed\"});");
    copied.insert({2, "changed"});
    trace("copied.erase(1);");
    copied.erase(1);
    check(original.find(1) != original.end(), "copy constructor shared erased node", 0, 1);
    check(original.find(2)->second == "two", "copy constructor shared value", 0, 2);

    HashMap<int, std::string> assigned;
    trace("assigned.insert({99, \"old\"});");
    assigned.insert({99, "old"});
    trace("assigned = original;");
    assigned = original;
    check(assigned.size() == original.size(), "copy assignment size");
    check(assigned.find(99) == assigned.end(), "copy assignment kept old key", 0, 99);
    trace("assigned.insert({3, \"assigned change\"});");
    assigned.insert({3, "assigned change"});
    check(original.find(3)->second == "three", "copy assignment shared value", 0, 3);

    HashMap<int, std::string>* original_self = &original;
    trace("original = *original_self;");
    original = *original_self;
    check(original.size() == 3, "self copy assignment size");
    check(original.find(1) != original.end(), "self copy assignment lost values", 0, 1);
}

void test_move_operations() {
    HashMap<int, int> source;
    trace("source.insert({1, 10});");
    source.insert({1, 10});
    trace("source.insert({2, 20});");
    source.insert({2, 20});

    HashMap<int, int> moved{std::move(source)};
    check(moved.size() == 2, "move constructor size");
    check(moved.find(1)->second == 10, "move constructor value", 0, 1);
    trace("source[7] = 70;");
    source[7] = 70;
    check(source.find(7)->second == 70, "moved-from reuse", 0, 7);

    HashMap<int, int> assigned;
    trace("assigned.insert({99, 990});");
    assigned.insert({99, 990});
    trace("assigned = std::move(moved);");
    assigned = std::move(moved);
    check(assigned.size() == 2, "move assignment size");
    check(assigned.find(1)->second == 10, "move assignment value", 0, 1);
    check(assigned.find(99) == assigned.end(), "move assignment kept old key", 0, 99);

    HashMap<int, int>* assigned_self = &assigned;
    trace("assigned = std::move(*assigned_self);");
    assigned = std::move(*assigned_self);
    check(assigned.size() == 2, "self move assignment size");
    check(assigned.find(2)->second == 20, "self move assignment lost values", 0, 2);
}

void test_value_lifetime() {
    check(Tracked::alive == 0, "tracked initial lifetime");

    {
        HashMap<int, Tracked> map;
        trace("map[1].value = 10;");
        map[1].value = 10;
        trace("map[2].value = 20;");
        map[2].value = 20;
        trace("map.insert({3, Tracked{30}});");
        map.insert({3, Tracked{30}});
        check(Tracked::alive == 3, "tracked live elements");
    }

    check(Tracked::alive == 0, "tracked final lifetime");
}

void test_randomized() {
    constexpr std::size_t operation_count = 50'000;
    constexpr unsigned seed = 0xC1A0'2026U;

    HashMap<int, int> actual;
    std::unordered_map<int, int> expected;
    std::mt19937 generator{seed};
    std::uniform_int_distribution<int> key_distribution{-300, 300};
    std::uniform_int_distribution<int> value_distribution{-100'000, 100'000};
    std::uniform_int_distribution<int> operation_distribution{0, 3};

    for (std::size_t step = 0; step < operation_count; ++step) {
        const int key = key_distribution(generator);
        const int value = value_distribution(generator);

        const int operation = operation_distribution(generator);
        trace("seed=0xC1A02026 step=" + std::to_string(step) +
              " operation=" + std::to_string(operation) +
              " (0=insert,1=subscript,2=erase,3=find) key=" + std::to_string(key) +
              " value=" + std::to_string(value));
        switch (operation) {
        case 0:
            trace("actual.insert({key, value});");
            actual.insert({key, value});
            expected.insert_or_assign(key, value);
            break;

        case 1:
            trace("actual[key] = value;");
            actual[key] = value;
            expected[key] = value;
            break;

        case 2: {
            trace("const std::size_t actual_removed = actual.erase(key);");
            const std::size_t actual_removed = actual.erase(key);
            const std::size_t expected_removed = expected.erase(key);
            check_equal(actual_removed, expected_removed, "random erase result", step, key);
            break;
        }

        case 3: {
            trace("auto actual_it = actual.find(key);");
            auto actual_it = actual.find(key);
            auto expected_it = expected.find(key);
            check_equal((actual_it == actual.end()), (expected_it == expected.end()),
                "random find presence",
                step,
                key);
            if (expected_it != expected.end()) {
                check_equal(actual_it->second, expected_it->second, "random find value", step, key);
            }
            break;
        }
        }

        if (step % 251 == 0) {
            check_against_reference(actual, expected, step);
        }
    }

    check_against_reference(actual, expected, operation_count);
}

struct Test { const char* name; void (*function)(); };

bool run_test(const Test& test) {
    current_test = test.name;
    *context = CrashContext{};
    std::cout << "[RUN] " << test.name << std::endl;
    std::cerr.flush();
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "[FAIL] " << test.name << ": fork: " << std::strerror(errno) << '\n';
        return false;
    }
    if (child == 0) {
        alarm(30);
        try {
            trace("enter test");
            test.function();
            std::cout.flush();
            std::cerr.flush();
            std::_Exit(0);
        } catch (const std::exception& error) {
            std::cerr << "[UNEXPECTED EXCEPTION] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            std::cerr << "[UNEXPECTED EXCEPTION] " << test.name << ": unknown exception\n";
        }
        std::cerr.flush();
        std::_Exit(1);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    const bool passed = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!passed) {
        if (waited < 0) std::cerr << "  waitpid: " << std::strerror(errno) << '\n';
        else if (WIFSIGNALED(status)) {
            std::cerr << "  Terminated by signal " << WTERMSIG(status)
                      << " (see assertion/sanitizer output above; signal 14 is the 30s timeout)\n";
        } else std::cerr << "  Child exit code: " << WEXITSTATUS(status) << '\n';
        print_recent_actions();
    }
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << std::endl;
    return passed;
}

} // namespace

int main(int argc, char** argv) {
    const Test tests[] = {
        {"empty_map", test_empty_map},
        {"insert_find_subscript", test_insert_find_and_subscript},
        {"collisions_iteration", test_collisions_and_iteration},
        {"automatic_rehash", test_automatic_rehash},
        {"explicit_rehash", test_explicit_rehash},
        {"erase_by_key", test_erase_by_key},
        {"erase_by_iterator", test_erase_by_iterator},
        {"copy_operations", test_copy_operations},
        {"move_operations", test_move_operations},
        {"value_lifetime", test_value_lifetime},
        {"randomized", test_randomized},
    };
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [--list|test_name]\n";
        return 2;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--list") {
        for (const auto& test : tests) std::cout << test.name << '\n';
        return 0;
    }
    void* storage = mmap(nullptr, sizeof(CrashContext), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANON, -1, 0);
    if (storage == MAP_FAILED) {
        std::cerr << "Cannot allocate crash context: " << std::strerror(errno) << '\n';
        return 2;
    }
    context = new (storage) CrashContext{};
    int ran = 0, failures = 0;
    for (const auto& test : tests) {
        if (argc == 2 && std::string_view(argv[1]) != test.name) continue;
        ++ran;
        failures += !run_test(test);
    }
    context->~CrashContext();
    munmap(storage, sizeof(CrashContext));
    if (ran == 0) {
        std::cerr << "Unknown test: " << argv[1] << ". Use --list.\n";
        return 2;
    }
    std::cout << "\nSummary: " << ran - failures << " passed, " << failures
              << " failed, " << ran << " total\n";
    return failures ? 1 : 0;
}
