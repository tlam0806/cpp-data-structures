// Build from this directory and run:
// clang++ -std=c++20 -g -fsanitize=address,undefined -fno-omit-frame-pointer test.cpp -o test && ./test
// Optional: ./test --list or ./test copy_constructor
// Tests run in child processes on macOS/Linux so one crash does not stop the suite.
//
// To check one API at a time (so a compile error in one API doesn't hide the rest),
// compile with -DONLY_TEST=<number>; the numbers are in the "#if ENABLE(n)" lines below.
#include "shared_ptr.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ONLY_TEST
#define ENABLE(n) 1
#else
#define ENABLE(n) (ONLY_TEST == (n))
#endif

// Lets a test make the next `new` throw, to check exception safety,
// and count allocations, to check make_shared allocates once and frees everything.
static bool fail_next_allocation = false;
static long total_allocations = 0;
static long live_allocations = 0;

void* operator new(std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) {
        ++total_allocations;
        ++live_allocations;
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if (p) --live_allocations;
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

template<class A, class B>
void equal(const A& actual, const B& expected, const char* description,
           std::source_location where = std::source_location::current()) {
    if (!(actual == expected)) {
        std::cerr << "[CHECK FAILED] " << current_test << '\n'
                  << "  " << where.file_name() << ':' << where.line() << '\n'
                  << "  " << description << "\n  Expected: " << expected
                  << "\n  Actual:   " << actual << '\n';
        std::cerr.flush();
        std::_Exit(1);
    }
}

struct Tracked {
    static inline int alive = 0;
    static inline int destroyed = 0;
    static inline int last_destroyed = -1;
    int id;
    explicit Tracked(int n) : id(n) { ++alive; }
    Tracked(const Tracked&) = delete;
    ~Tracked() { --alive; ++destroyed; last_destroyed = id; }
};

// Polymorphic hierarchy: conversions are safe even if deletion goes through Base*.
struct Base {
    static inline int destroyed = 0;
    virtual ~Base() { ++destroyed; }
};
struct Derived : Base {
    static inline int destroyed = 0;
    long payload[4]{};
    ~Derived() override { ++destroyed; }
};

// Non-virtual hierarchy: only correct if the control block remembers the real type.
struct PlainBase {
    static inline int destroyed = 0;
    ~PlainBase() { ++destroyed; }
};
struct PlainDerived : PlainBase {
    static inline int destroyed = 0;
    long payload[4]{};
    ~PlainDerived() { ++destroyed; }
};

// shared_ptr has no use_count() yet, so read the strong count through a weak_ptr.
template<class T>
long strong_count(const shared_ptr<T>& p) {
    return static_cast<long>(weak_ptr<T>(p).use_count());
}

// ---------------------------------------------------------------- compile-time API

#if ENABLE(1)
static_assert(!std::is_convertible_v<Tracked*, shared_ptr<Tracked>>,
              "raw pointer constructor must be explicit");
static_assert(std::is_constructible_v<shared_ptr<Base>, const shared_ptr<Derived>&>,
              "Derived -> Base conversion must be allowed");
static_assert(!std::is_constructible_v<shared_ptr<Derived>, const shared_ptr<Base>&>,
              "Base -> Derived conversion must be rejected");
static_assert(!std::is_constructible_v<shared_ptr<int>, const shared_ptr<double>&>,
              "unrelated conversion must be rejected");
static_assert(std::is_nothrow_copy_constructible_v<shared_ptr<Tracked>>);
static_assert(std::is_nothrow_move_constructible_v<shared_ptr<Tracked>>);
static_assert(std::is_same_v<decltype(std::declval<shared_ptr<Tracked>&>() =
                                      std::declval<const shared_ptr<Tracked>&>()),
                             shared_ptr<Tracked>&>,
              "assignment must return a reference");
static_assert(!noexcept(std::declval<shared_ptr<Tracked>&>().reset(std::declval<Tracked*>())),
              "reset(p) allocates, so it must not be noexcept");
void compile_time_api() {}
#endif

// ---------------------------------------------------------------- shared_ptr

#if ENABLE(2)
void raw_pointer_constructor() {
    {
        shared_ptr<Tracked> p(new Tracked(1));
        equal(Tracked::alive, 1, "object is alive while owned");
        equal(strong_count(p), 1L, "one owner");
    }
    equal(Tracked::destroyed, 1, "object destroyed exactly once at scope exit");
}
#endif

#if ENABLE(3)
void raw_pointer_constructor_remembers_real_type() {
    { shared_ptr<PlainBase> p(new PlainDerived); }
    equal(PlainDerived::destroyed, 1, "~PlainDerived must run although ~PlainBase is not virtual");
    equal(PlainBase::destroyed, 1, "base part destroyed once");
}
#endif

#if ENABLE(4)
void raw_pointer_constructor_allocation_failure() {
    auto* raw = new Tracked(1);
    bool threw = false;
    fail_next_allocation = true;
    try {
        shared_ptr<Tracked> p(raw);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
    equal(Tracked::alive, 0, "constructor must delete its pointer if the control block allocation throws");
}
#endif

#if ENABLE(5)
void copy_constructor() {
    {
        shared_ptr<Tracked> a(new Tracked(1));
        {
            shared_ptr<Tracked> b(a);
            equal(strong_count(a), 2L, "copy adds an owner");
        }
        equal(Tracked::alive, 1, "object survives while a still owns it");
        equal(strong_count(a), 1L, "copy's destructor removed its owner");
    }
    equal(Tracked::destroyed, 1, "object destroyed exactly once");
}
#endif

#if ENABLE(6)
void converting_copy_constructor() {
    {
        shared_ptr<Derived> d(new Derived);
        {
            shared_ptr<Base> b(d);
            equal(strong_count(d), 2L, "converted copy shares the control block");
        }
        equal(Derived::destroyed, 0, "object survives while d still owns it");
    }
    equal(Derived::destroyed, 1, "object destroyed exactly once");
}
#endif

#if ENABLE(7)
void move_constructor() {
    {
        shared_ptr<Tracked> a(new Tracked(1));
        shared_ptr<Tracked> b(std::move(a));
        equal(strong_count(b), 1L, "move does not add an owner");
        equal(strong_count(a), 0L, "moved-from pointer is empty");
        equal(Tracked::alive, 1, "object still alive");
    }
    equal(Tracked::destroyed, 1, "object destroyed exactly once");
}
#endif

#if ENABLE(8)
void converting_move_constructor() {
    {
        shared_ptr<Derived> d(new Derived);
        shared_ptr<Base> b(std::move(d));
        equal(strong_count(b), 1L, "move does not add an owner");
        equal(strong_count(d), 0L, "moved-from pointer is empty");
    }
    equal(Derived::destroyed, 1, "object destroyed exactly once");
}
#endif

#if ENABLE(9)
void copy_assignment() {
    {
        shared_ptr<Tracked> a(new Tracked(1));
        shared_ptr<Tracked> b(new Tracked(2));
        b = a;
        equal(Tracked::destroyed, 1, "b's old object destroyed");
        equal(Tracked::last_destroyed, 2, "the destroyed object is b's old one");
        equal(strong_count(a), 2L, "a and b share ownership");
    }
    equal(Tracked::destroyed, 2, "both objects destroyed exactly once");
}
#endif

#if ENABLE(10)
void converting_copy_assignment() {
    {
        shared_ptr<Base> b(new Base);
        shared_ptr<Derived> d(new Derived);
        b = d;
        equal(Base::destroyed, 1, "b's old Base destroyed");
        equal(Derived::destroyed, 0, "Derived still owned by d and b");
        equal(strong_count(d), 2L, "d and b share ownership");
    }
    equal(Derived::destroyed, 1, "Derived destroyed exactly once");
    equal(Base::destroyed, 2, "both Base subobjects destroyed");
}
#endif

#if ENABLE(11)
void move_assignment() {
    {
        shared_ptr<Tracked> a(new Tracked(1));
        shared_ptr<Tracked> b(new Tracked(2));
        b = std::move(a);
        equal(Tracked::last_destroyed, 2, "b's old object destroyed");
        equal(strong_count(b), 1L, "b is the only owner");
        equal(strong_count(a), 0L, "moved-from pointer is empty");
    }
    equal(Tracked::destroyed, 2, "both objects destroyed exactly once");
}
#endif

#if ENABLE(12)
void self_assignment() {
    {
        shared_ptr<Tracked> a(new Tracked(1));
        auto& alias = a;
        a = alias;
        equal(Tracked::alive, 1, "self-assignment keeps the object");
        equal(strong_count(a), 1L, "self-assignment keeps the count");
    }
    equal(Tracked::destroyed, 1, "object destroyed exactly once");
}
#endif

#if ENABLE(13)
void swap_owners() {
    {
        shared_ptr<Tracked> a(new Tracked(1));
        shared_ptr<Tracked> b(new Tracked(2));
        shared_ptr<Tracked> extra(b);
        a.swap(b);
        equal(strong_count(a), 2L, "a now shares object 2 with extra");
        equal(strong_count(b), 1L, "b now owns object 1 alone");
        { shared_ptr<Tracked> sink(std::move(b)); }
        equal(Tracked::last_destroyed, 1, "b held object 1 after swap");
    }
    equal(Tracked::destroyed, 2, "both objects destroyed exactly once");
}
#endif

#if ENABLE(14)
void reset_to_empty() {
    shared_ptr<Tracked> a(new Tracked(1));
    a.reset();
    equal(Tracked::destroyed, 1, "reset() destroys the last owner's object");
    equal(strong_count(a), 0L, "pointer is empty after reset()");
}
#endif

#if ENABLE(15)
void reset_with_pointer() {
    {
        shared_ptr<Tracked> a(new Tracked(1));
        a.reset(new Tracked(2));
        equal(Tracked::last_destroyed, 1, "old object destroyed");
        equal(Tracked::alive, 1, "new object alive");
        equal(strong_count(a), 1L, "new object has one owner");
    }
    equal(Tracked::destroyed, 2, "both objects destroyed exactly once");
}
#endif

#if ENABLE(16)
void reset_with_pointer_on_empty() {
    {
        shared_ptr<Tracked> a(new Tracked(1));
        shared_ptr<Tracked> keep(std::move(a));
        a.reset(new Tracked(2));
        equal(strong_count(a), 1L, "empty pointer takes ownership");
        equal(Tracked::alive, 2, "nothing destroyed");
    }
    equal(Tracked::destroyed, 2, "both objects destroyed exactly once");
}
#endif

// ---------------------------------------------------------------- weak_ptr

#if ENABLE(17)
void weak_from_shared() {
    {
        weak_ptr<Tracked> w = [] {
            shared_ptr<Tracked> p(new Tracked(1));
            weak_ptr<Tracked> w(p);
            equal(w.use_count(), std::size_t{1}, "weak_ptr does not add a strong owner");
            CHECK(!w.expired());
            return w;
        }();
        equal(Tracked::destroyed, 1, "object dies with the last shared_ptr, not the weak_ptr");
        CHECK(w.expired());
        equal(w.use_count(), std::size_t{0}, "use_count is 0 after expiry");
    }
    // Leaving scope frees the control block; ASan reports any double free here.
}
#endif

#if ENABLE(18)
void weak_copy_constructor() {
    shared_ptr<Tracked> p(new Tracked(1));
    weak_ptr<Tracked> w1(p);
    {
        weak_ptr<Tracked> w2(w1);
        equal(w2.use_count(), std::size_t{1}, "copy observes the same object");
    }
    p.reset();
    CHECK(w1.expired());
    // w1 must still be able to read the control block: ASan flags use-after-free if
    // the copy didn't take its own weak reference.
}
#endif

#if ENABLE(19)
void weak_converting_constructors() {
    shared_ptr<Derived> d(new Derived);
    weak_ptr<Derived> wd(d);
    weak_ptr<Base> from_weak(wd);
    weak_ptr<Base> from_shared(d);
    equal(from_weak.use_count(), std::size_t{1}, "converted from weak_ptr<Derived>");
    equal(from_shared.use_count(), std::size_t{1}, "converted from shared_ptr<Derived>");
    d.reset();
    CHECK(from_weak.expired());
    CHECK(from_shared.expired());
    equal(Derived::destroyed, 1, "object destroyed exactly once");
}
#endif

#if ENABLE(20)
void weak_move_constructor() {
    shared_ptr<Tracked> p(new Tracked(1));
    weak_ptr<Tracked> w1(p);
    weak_ptr<Tracked> w2(std::move(w1));
    equal(w2.use_count(), std::size_t{1}, "moved-to observes the object");
    equal(w1.use_count(), std::size_t{0}, "moved-from is empty");
    CHECK(w1.expired());
}
#endif

#if ENABLE(21)
void weak_copy_assignment() {
    shared_ptr<Tracked> a(new Tracked(1));
    shared_ptr<Tracked> b(new Tracked(2));
    weak_ptr<Tracked> wa(a);
    weak_ptr<Tracked> wb(b);
    wb = wa;
    b.reset();
    equal(wb.use_count(), std::size_t{1}, "wb now observes a's object");
    a.reset();
    CHECK(wa.expired());
    CHECK(wb.expired());
}
#endif

#if ENABLE(22)
void weak_converting_assignment() {
    shared_ptr<Derived> d(new Derived);
    shared_ptr<Base> b(new Base);
    weak_ptr<Derived> wd(d);
    weak_ptr<Base> wb(b);
    wb = wd;
    b.reset();
    equal(wb.use_count(), std::size_t{1}, "wb now observes d's object");
    d.reset();
    CHECK(wb.expired());
}
#endif

#if ENABLE(23)
void lock_alive() {
    shared_ptr<Tracked> p(new Tracked(1));
    weak_ptr<Tracked> w(p);
    shared_ptr<Tracked> locked = w.lock();
    equal(w.use_count(), std::size_t{2}, "lock() adds a strong owner");
    p.reset();
    equal(Tracked::alive, 1, "locked pointer keeps the object alive");
    locked.reset();
    equal(Tracked::destroyed, 1, "object destroyed once the locked owner goes away");
    CHECK(w.expired());
}
#endif

#if ENABLE(24)
void lock_expired() {
    shared_ptr<Tracked> p(new Tracked(1));
    weak_ptr<Tracked> w(p);
    p.reset();
    shared_ptr<Tracked> locked = w.lock();
    equal(strong_count(locked), 0L, "lock() on an expired weak_ptr returns empty");
    equal(Tracked::destroyed, 1, "object is not resurrected");
}
#endif

#if ENABLE(25)
void weak_reset() {
    shared_ptr<Tracked> p(new Tracked(1));
    weak_ptr<Tracked> w(p);
    w.reset();
    CHECK(w.expired());
    equal(Tracked::alive, 1, "reset() on weak_ptr does not destroy the object");
}
#endif

#if ENABLE(26)
void weak_swap() {
    shared_ptr<Tracked> a(new Tracked(1));
    shared_ptr<Tracked> b(new Tracked(2));
    shared_ptr<Tracked> b2(b);
    weak_ptr<Tracked> wa(a);
    weak_ptr<Tracked> wb(b);
    wa.swap(wb);
    equal(wa.use_count(), std::size_t{2}, "wa now observes object 2");
    equal(wb.use_count(), std::size_t{1}, "wb now observes object 1");
}
#endif

// ---------------------------------------------------------------- custom deleter

// Records how it was called; deletes the object itself so nothing leaks.
struct CountingDeleter {
    int* calls;
    Tracked** received;
    void operator()(Tracked* p) const {
        ++*calls;
        *received = p;
        delete p;
    }
};

struct EmptyDeleter {
    void operator()(int* p) const { delete p; }
};

#if ENABLE(27)
static_assert(!std::is_constructible_v<shared_ptr<Tracked>, Tracked*, int>,
              "a non-callable second argument must not be accepted as a deleter");
static_assert(!std::is_constructible_v<shared_ptr<Tracked>, Tracked*, void (*)(int*)>,
              "a deleter that can't take U* must be rejected");
static_assert(std::is_constructible_v<shared_ptr<Tracked>, Tracked*, CountingDeleter>);
static_assert(sizeof(detail::ControlBlockFor<int, EmptyDeleter>) ==
              sizeof(detail::ControlBlockFor<int>),
              "an empty deleter must not make the control block bigger");
void deleter_compile_time_api() {}
#endif

#if ENABLE(28)
void deleter_called_once_with_pointer() {
    int calls = 0;
    Tracked* received = nullptr;
    auto* raw = new Tracked(1);
    {
        shared_ptr<Tracked> p(raw, CountingDeleter{&calls, &received});
        equal(calls, 0, "deleter not called while owned");
    }
    equal(calls, 1, "deleter called exactly once");
    CHECK(received == raw);
    equal(Tracked::destroyed, 1, "deleter destroyed the object");
}
#endif

#if ENABLE(29)
void deleter_lambda_with_capture() {
    int calls = 0;
    {
        shared_ptr<Tracked> p(new Tracked(1), [&calls](Tracked* t) {
            ++calls;
            delete t;
        });
    }
    equal(calls, 1, "capturing lambda called once");
    equal(Tracked::destroyed, 1, "object destroyed");
}
#endif

#if ENABLE(30)
void deleter_lvalue_is_copied() {
    int calls = 0;
    Tracked* received = nullptr;
    CountingDeleter d{&calls, &received};
    {
        shared_ptr<Tracked> p(new Tracked(1), d);
    }
    equal(calls, 1, "copied deleter called once");
    CHECK(d.calls == &calls);   // caller's deleter is still usable
}
#endif

#if ENABLE(31)
void deleter_waits_for_last_owner() {
    int calls = 0;
    Tracked* received = nullptr;
    {
        shared_ptr<Tracked> a(new Tracked(1), CountingDeleter{&calls, &received});
        {
            shared_ptr<Tracked> b(a);
            shared_ptr<const Tracked> c(a);
        }
        equal(calls, 0, "copies dying doesn't call the deleter");
    }
    equal(calls, 1, "deleter called once when the last owner dies");
}
#endif

#if ENABLE(32)
void deleter_runs_while_weak_alive() {
    int calls = 0;
    Tracked* received = nullptr;
    shared_ptr<Tracked> p(new Tracked(1), CountingDeleter{&calls, &received});
    weak_ptr<Tracked> w(p);
    p.reset();
    equal(calls, 1, "deleter runs when the strong count hits 0, not when weak does");
    CHECK(w.expired());
}
#endif

#if ENABLE(33)
void deleter_receives_original_type() {
    int calls = 0;
    {
        shared_ptr<PlainBase> p(new PlainDerived, [&calls](PlainDerived* d) {
            ++calls;
            delete d;
        });
    }
    equal(calls, 1, "deleter called once");
    equal(PlainDerived::destroyed, 1, "deleter got a PlainDerived*, so ~PlainDerived ran");
}
#endif

#if ENABLE(34)
void deleter_for_non_new_memory() {
    int calls = 0;
    {
        auto* raw = static_cast<int*>(std::malloc(sizeof(int)));
        *raw = 42;
        shared_ptr<int> p(raw, [&calls](int* q) {
            ++calls;
            std::free(q);
        });
    }
    equal(calls, 1, "free() used instead of delete");
}
#endif

#if ENABLE(35)
void deleter_called_on_allocation_failure() {
    int calls = 0;
    Tracked* received = nullptr;
    auto* raw = new Tracked(1);
    bool threw = false;
    fail_next_allocation = true;
    try {
        shared_ptr<Tracked> p(raw, CountingDeleter{&calls, &received});
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
    equal(calls, 1, "the caller's deleter (not delete) cleans up when allocation throws");
    CHECK(received == raw);
    equal(Tracked::alive, 0, "object freed");
}
#endif

#if ENABLE(36)
void reset_then_custom_deleter_still_used() {
    int calls = 0;
    Tracked* received = nullptr;
    shared_ptr<Tracked> p(new Tracked(1), CountingDeleter{&calls, &received});
    shared_ptr<Tracked> q(std::move(p));
    shared_ptr<Tracked> r;
    r = q;
    q.reset();
    equal(calls, 0, "r still owns the object");
    r.reset();
    equal(calls, 1, "deleter travels with the control block through move and assignment");
}
#endif

// ---------------------------------------------------------------- observers

template<class P>
concept CanTestInIf = requires(const P& p) { p ? 1 : 0; };

#if ENABLE(37)
using SP = shared_ptr<Tracked>;
static_assert(std::is_same_v<decltype(std::declval<const SP&>().get()), Tracked*>,
              "get() is const and returns T*");
static_assert(std::is_same_v<decltype(*std::declval<const SP&>()), Tracked&>,
              "operator* is const and returns T&");
static_assert(std::is_same_v<decltype(std::declval<const SP&>().operator->()), Tracked*>,
              "operator-> is const and returns T*");
static_assert(std::is_same_v<decltype(std::declval<const SP&>().use_count()), std::size_t>,
              "use_count() is const");
static_assert(CanTestInIf<SP>, "operator bool must be callable on a const shared_ptr");
static_assert(!std::is_convertible_v<SP, bool>, "operator bool must be explicit");
static_assert(noexcept(std::declval<const SP&>().get()));
static_assert(noexcept(std::declval<const SP&>().use_count()));
static_assert(std::is_same_v<decltype(std::declval<shared_ptr<Base>&>() =
                                      std::declval<const shared_ptr<Derived>&>()),
                             shared_ptr<Base>&>,
              "converting assignment must return a reference, not a copy");
static_assert(noexcept(std::declval<weak_ptr<Tracked>&>().reset()),
              "weak_ptr::reset() never allocates, so it should be noexcept");
static_assert(std::is_same_v<decltype(std::declval<const weak_ptr<Tracked>&>().lock()),
                             shared_ptr<Tracked>>,
              "lock() is const");
void observers_compile_time_api() {}
#endif

#if ENABLE(38)
void get_returns_pointer() {
    auto* raw = new Tracked(1);
    shared_ptr<Tracked> p(raw);
    CHECK(p.get() == raw);
    shared_ptr<Tracked> empty;
    CHECK(empty.get() == nullptr);
    auto* derived = new Derived;
    shared_ptr<Derived> d(derived);
    shared_ptr<Base> b(d);
    CHECK(b.get() == static_cast<Base*>(derived));
}
#endif

#if ENABLE(39)
void dereference() {
    const shared_ptr<Tracked> p(new Tracked(1));
    equal((*p).id, 1, "*p reads the object");
    (*p).id = 5;   // const shared_ptr, non-const object
    equal(p.get()->id, 5, "*p writes the object");
}
#endif

#if ENABLE(40)
void arrow() {
    const shared_ptr<Tracked> p(new Tracked(1));
    equal(p->id, 1, "p->id reads the object");
    p->id = 7;
    equal(p.get()->id, 7, "p->id writes the object");
}
#endif

#if ENABLE(41)
void use_count_tracks_owners() {
    shared_ptr<Tracked> empty;
    equal(empty.use_count(), std::size_t{0}, "empty has no owners");
    shared_ptr<Tracked> a(new Tracked(1));
    equal(a.use_count(), std::size_t{1}, "one owner");
    shared_ptr<Tracked> b(a);
    equal(a.use_count(), std::size_t{2}, "copy adds an owner");
    equal(b.use_count(), std::size_t{2}, "both owners see the same count");
    shared_ptr<Tracked> c(std::move(b));
    equal(a.use_count(), std::size_t{2}, "move doesn't change the count");
    equal(b.use_count(), std::size_t{0}, "moved-from has no owners");
    c.reset();
    equal(a.use_count(), std::size_t{1}, "reset removes an owner");
}
#endif

#if ENABLE(42)
void operator_bool() {
    shared_ptr<Tracked> empty;
    CHECK(!empty);
    shared_ptr<Tracked> p(new Tracked(1));
    CHECK(static_cast<bool>(p));
    shared_ptr<Tracked> q(p);
    shared_ptr<Tracked> moved(std::move(q));
    CHECK(!q);
    CHECK(static_cast<bool>(moved));
    moved.reset();
    CHECK(!moved);
}
#endif

#if ENABLE(43)
void operator_bool_owning_nullptr() {
    // std::shared_ptr: bool means get() != nullptr, even if a control block exists.
    shared_ptr<Tracked> p(static_cast<Tracked*>(nullptr));
    equal(p.use_count(), std::size_t{1}, "a null pointer still gets a control block");
    CHECK(p.get() == nullptr);
    CHECK(!p);
}
#endif

#if ENABLE(44)
void accessors_after_lock() {
    shared_ptr<Tracked> p(new Tracked(3));
    weak_ptr<Tracked> w(p);
    shared_ptr<Tracked> locked = w.lock();
    CHECK(static_cast<bool>(locked));
    CHECK(locked.get() == p.get());
    equal(locked->id, 3, "locked pointer reaches the object");
    equal(locked.use_count(), std::size_t{2}, "lock adds an owner");
}
#endif

// ---------------------------------------------------------------- nullptr and comparisons

#if ENABLE(45)
static_assert(std::is_convertible_v<std::nullptr_t, shared_ptr<int>>,
              "shared_ptr<int> p = nullptr; must compile (constructor is not explicit)");
static_assert(std::is_nothrow_constructible_v<shared_ptr<int>, std::nullptr_t>);
static_assert(std::is_assignable_v<shared_ptr<int>&, std::nullptr_t>, "p = nullptr must compile");
static_assert(std::is_same_v<decltype(std::declval<const shared_ptr<int>&>() == nullptr), bool>);
static_assert(std::is_same_v<decltype(nullptr == std::declval<const shared_ptr<int>&>()), bool>,
              "nullptr == p must compile");
static_assert(std::is_same_v<decltype(std::declval<const shared_ptr<int>&>() != nullptr), bool>,
              "p != nullptr must compile");
static_assert(std::is_same_v<decltype(std::declval<const shared_ptr<Base>&>() ==
                                      std::declval<const shared_ptr<Derived>&>()), bool>,
              "comparison across convertible types must compile");
void nullptr_compile_time_api() {}
#endif

#if ENABLE(46)
void construct_from_nullptr() {
    shared_ptr<Tracked> p(nullptr);
    shared_ptr<Tracked> q = nullptr;
    CHECK(p.get() == nullptr);
    CHECK(q.get() == nullptr);
    equal(p.use_count(), std::size_t{0}, "nullptr constructor creates no control block");
    CHECK(!p);
}
#endif

#if ENABLE(47)
void assign_nullptr() {
    shared_ptr<Tracked> p(new Tracked(1));
    p = nullptr;
    equal(Tracked::destroyed, 1, "assigning nullptr releases the object");
    CHECK(p.get() == nullptr);
    equal(p.use_count(), std::size_t{0}, "pointer is empty");
}
#endif

#if ENABLE(48)
void compare_with_nullptr() {
    shared_ptr<Tracked> empty;
    shared_ptr<Tracked> p(new Tracked(1));
    CHECK(empty == nullptr);
    CHECK(nullptr == empty);
    CHECK(!(empty != nullptr));
    CHECK(p != nullptr);
    CHECK(nullptr != p);
    CHECK(!(p == nullptr));
}
#endif

#if ENABLE(49)
void compare_shared_ptrs() {
    shared_ptr<Tracked> a(new Tracked(1));
    shared_ptr<Tracked> copy(a);
    shared_ptr<Tracked> b(new Tracked(2));
    shared_ptr<Tracked> empty1, empty2;
    CHECK(a == copy);
    CHECK(!(a != copy));
    CHECK(a != b);
    CHECK(empty1 == empty2);
    CHECK(a != empty1);

    shared_ptr<Derived> d(new Derived);
    shared_ptr<Base> base_view(d);
    CHECK(base_view == d);
    CHECK(d == base_view);
}
#endif

// ---------------------------------------------------------------- reset with deleter

#if ENABLE(50)
void reset_with_deleter() {
    int calls = 0;
    Tracked* received = nullptr;
    {
        shared_ptr<Tracked> p(new Tracked(1));
        auto* raw = new Tracked(2);
        p.reset(raw, CountingDeleter{&calls, &received});
        equal(Tracked::last_destroyed, 1, "old object destroyed with plain delete");
        equal(calls, 0, "new deleter not called yet");
        CHECK(p.get() == raw);
        equal(p.use_count(), std::size_t{1}, "one owner");
    }
    equal(calls, 1, "new deleter called once at the end");
    equal(Tracked::destroyed, 2, "both objects destroyed exactly once");
}
#endif

#if ENABLE(51)
void reset_with_deleter_allocation_failure() {
    int calls = 0;
    Tracked* received = nullptr;
    shared_ptr<Tracked> p(new Tracked(1));
    auto* raw = new Tracked(2);
    bool threw = false;
    fail_next_allocation = true;
    try {
        p.reset(raw, CountingDeleter{&calls, &received});
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
    equal(calls, 1, "deleter cleans up the new pointer when allocation throws");
    CHECK(received == raw);
    equal(p->id, 1, "p still owns its old object (strong guarantee)");
    equal(p.use_count(), std::size_t{1}, "old ownership untouched");
}
#endif

// ---------------------------------------------------------------- make_shared

struct Aligned {
    alignas(64) unsigned char data[64]{};
};

struct ThrowsOnConstruct {
    static inline int destroyed = 0;
    explicit ThrowsOnConstruct(int) { throw std::runtime_error("construction failed"); }
    ~ThrowsOnConstruct() { ++destroyed; }
};

struct Forwarded {
    int& ref;
    std::unique_ptr<int> owned;
    Forwarded(int& r, std::unique_ptr<int> o) : ref(r), owned(std::move(o)) {}
};

#if ENABLE(52)
static_assert(std::is_same_v<decltype(::make_shared<Tracked>(1)), shared_ptr<Tracked>>,
              "make_shared<T> returns shared_ptr<T>");
void make_shared_compile_time_api() {}
#endif

#if ENABLE(53)
void make_shared_constructs_object() {
    {
        auto p = ::make_shared<Tracked>(7);
        equal(p->id, 7, "constructor arguments forwarded");
        equal(Tracked::alive, 1, "one object constructed");
        equal(p.use_count(), std::size_t{1}, "make_shared result has exactly one owner");
    }
    equal(Tracked::destroyed, 1, "object destroyed exactly once");
}
#endif

#if ENABLE(54)
void make_shared_single_allocation() {
    long before_total = total_allocations;
    long before_live = live_allocations;
    {
        auto p = ::make_shared<Tracked>(1);
        equal(total_allocations - before_total, 1L, "object and control block share one allocation");
    }
    equal(live_allocations - before_live, 0L, "the combined block is freed");
}
#endif

#if ENABLE(55)
void make_shared_forwards_arguments() {
    int x = 1;
    auto p = ::make_shared<Forwarded>(x, std::make_unique<int>(5));
    CHECK(&p->ref == &x);   // lvalue stays an lvalue reference
    equal(*p->owned, 5, "move-only argument forwarded as rvalue");
}
#endif

#if ENABLE(56)
void make_shared_respects_alignment() {
    auto p = ::make_shared<Aligned>();
    equal(reinterpret_cast<std::uintptr_t>(p.get()) % alignof(Aligned), std::uintptr_t{0},
          "object is correctly aligned inside the control block");
}
#endif

#if ENABLE(57)
void make_shared_throwing_constructor() {
    long before_live = live_allocations;
    bool threw = false;
    try {
        auto p = ::make_shared<ThrowsOnConstruct>(1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    equal(ThrowsOnConstruct::destroyed, 0, "a never-constructed object is not destroyed");
    equal(live_allocations - before_live, 0L, "the block's memory is freed when T's constructor throws");
}
#endif

#if ENABLE(58)
void make_shared_with_weak_ptr() {
    long before_live = live_allocations;
    {
        weak_ptr<Tracked> w;
        {
            auto p = ::make_shared<Tracked>(1);
            w = p;
        }
        equal(Tracked::destroyed, 1, "object destroyed when the last shared_ptr dies");
        CHECK(w.expired());
        CHECK(live_allocations - before_live == 1);   // block kept alive by w
    }
    equal(live_allocations - before_live, 0L, "block freed when the last weak_ptr dies");
}
#endif

#if ENABLE(59)
void make_shared_converts_to_base() {
    {
        shared_ptr<Base> b = ::make_shared<Derived>();
        equal(b.use_count(), std::size_t{1}, "one owner after conversion");
    }
    equal(Derived::destroyed, 1, "~Derived runs");
}
#endif

struct Test { const char* name; void (*run)(); };
bool run_test(const Test& test) {
    current_test = test.name;
    std::cout << "[RUN] " << test.name << std::endl;
    std::cerr.flush();
    pid_t child = fork();
    if (child < 0) { std::cerr << "fork failed\n"; return false; }
    if (child == 0) {
        alarm(30);
        try {
            test.run();
            equal(Tracked::alive, 0, "no objects leaked by test");
            std::cout.flush(); std::cerr.flush();
            std::_Exit(0);
        } catch (const std::exception& e) {
            std::cerr << "[UNEXPECTED EXCEPTION] " << test.name << ": " << e.what() << '\n';
        } catch (...) { std::cerr << "[UNEXPECTED EXCEPTION] " << test.name << '\n'; }
        std::cerr.flush(); std::_Exit(1);
    }
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    bool passed = result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name;
    if (result == child && WIFSIGNALED(status)) std::cout << " (signal " << WTERMSIG(status) << ')';
    std::cout << std::endl;
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    const Test tests[] = {
#if ENABLE(1)
        {"compile_time_api", compile_time_api},
#endif
#if ENABLE(2)
        {"raw_pointer_constructor", raw_pointer_constructor},
#endif
#if ENABLE(3)
        {"raw_pointer_constructor_remembers_real_type", raw_pointer_constructor_remembers_real_type},
#endif
#if ENABLE(4)
        {"raw_pointer_constructor_allocation_failure", raw_pointer_constructor_allocation_failure},
#endif
#if ENABLE(5)
        {"copy_constructor", copy_constructor},
#endif
#if ENABLE(6)
        {"converting_copy_constructor", converting_copy_constructor},
#endif
#if ENABLE(7)
        {"move_constructor", move_constructor},
#endif
#if ENABLE(8)
        {"converting_move_constructor", converting_move_constructor},
#endif
#if ENABLE(9)
        {"copy_assignment", copy_assignment},
#endif
#if ENABLE(10)
        {"converting_copy_assignment", converting_copy_assignment},
#endif
#if ENABLE(11)
        {"move_assignment", move_assignment},
#endif
#if ENABLE(12)
        {"self_assignment", self_assignment},
#endif
#if ENABLE(13)
        {"swap_owners", swap_owners},
#endif
#if ENABLE(14)
        {"reset_to_empty", reset_to_empty},
#endif
#if ENABLE(15)
        {"reset_with_pointer", reset_with_pointer},
#endif
#if ENABLE(16)
        {"reset_with_pointer_on_empty", reset_with_pointer_on_empty},
#endif
#if ENABLE(17)
        {"weak_from_shared", weak_from_shared},
#endif
#if ENABLE(18)
        {"weak_copy_constructor", weak_copy_constructor},
#endif
#if ENABLE(19)
        {"weak_converting_constructors", weak_converting_constructors},
#endif
#if ENABLE(20)
        {"weak_move_constructor", weak_move_constructor},
#endif
#if ENABLE(21)
        {"weak_copy_assignment", weak_copy_assignment},
#endif
#if ENABLE(22)
        {"weak_converting_assignment", weak_converting_assignment},
#endif
#if ENABLE(23)
        {"lock_alive", lock_alive},
#endif
#if ENABLE(24)
        {"lock_expired", lock_expired},
#endif
#if ENABLE(25)
        {"weak_reset", weak_reset},
#endif
#if ENABLE(26)
        {"weak_swap", weak_swap},
#endif
#if ENABLE(27)
        {"deleter_compile_time_api", deleter_compile_time_api},
#endif
#if ENABLE(28)
        {"deleter_called_once_with_pointer", deleter_called_once_with_pointer},
#endif
#if ENABLE(29)
        {"deleter_lambda_with_capture", deleter_lambda_with_capture},
#endif
#if ENABLE(30)
        {"deleter_lvalue_is_copied", deleter_lvalue_is_copied},
#endif
#if ENABLE(31)
        {"deleter_waits_for_last_owner", deleter_waits_for_last_owner},
#endif
#if ENABLE(32)
        {"deleter_runs_while_weak_alive", deleter_runs_while_weak_alive},
#endif
#if ENABLE(33)
        {"deleter_receives_original_type", deleter_receives_original_type},
#endif
#if ENABLE(34)
        {"deleter_for_non_new_memory", deleter_for_non_new_memory},
#endif
#if ENABLE(35)
        {"deleter_called_on_allocation_failure", deleter_called_on_allocation_failure},
#endif
#if ENABLE(36)
        {"reset_then_custom_deleter_still_used", reset_then_custom_deleter_still_used},
#endif
#if ENABLE(37)
        {"observers_compile_time_api", observers_compile_time_api},
#endif
#if ENABLE(38)
        {"get_returns_pointer", get_returns_pointer},
#endif
#if ENABLE(39)
        {"dereference", dereference},
#endif
#if ENABLE(40)
        {"arrow", arrow},
#endif
#if ENABLE(41)
        {"use_count_tracks_owners", use_count_tracks_owners},
#endif
#if ENABLE(42)
        {"operator_bool", operator_bool},
#endif
#if ENABLE(43)
        {"operator_bool_owning_nullptr", operator_bool_owning_nullptr},
#endif
#if ENABLE(44)
        {"accessors_after_lock", accessors_after_lock},
#endif
#if ENABLE(45)
        {"nullptr_compile_time_api", nullptr_compile_time_api},
#endif
#if ENABLE(46)
        {"construct_from_nullptr", construct_from_nullptr},
#endif
#if ENABLE(47)
        {"assign_nullptr", assign_nullptr},
#endif
#if ENABLE(48)
        {"compare_with_nullptr", compare_with_nullptr},
#endif
#if ENABLE(49)
        {"compare_shared_ptrs", compare_shared_ptrs},
#endif
#if ENABLE(50)
        {"reset_with_deleter", reset_with_deleter},
#endif
#if ENABLE(51)
        {"reset_with_deleter_allocation_failure", reset_with_deleter_allocation_failure},
#endif
#if ENABLE(52)
        {"make_shared_compile_time_api", make_shared_compile_time_api},
#endif
#if ENABLE(53)
        {"make_shared_constructs_object", make_shared_constructs_object},
#endif
#if ENABLE(54)
        {"make_shared_single_allocation", make_shared_single_allocation},
#endif
#if ENABLE(55)
        {"make_shared_forwards_arguments", make_shared_forwards_arguments},
#endif
#if ENABLE(56)
        {"make_shared_respects_alignment", make_shared_respects_alignment},
#endif
#if ENABLE(57)
        {"make_shared_throwing_constructor", make_shared_throwing_constructor},
#endif
#if ENABLE(58)
        {"make_shared_with_weak_ptr", make_shared_with_weak_ptr},
#endif
#if ENABLE(59)
        {"make_shared_converts_to_base", make_shared_converts_to_base},
#endif
    };
    if (argc > 2) { std::cerr << "Usage: " << argv[0] << " [--list|test_name]\n"; return 2; }
    if (argc == 2 && std::string_view(argv[1]) == "--list") {
        for (const auto& t : tests) std::cout << t.name << '\n';
        return 0;
    }
    int count = 0, failures = 0;
    for (const auto& t : tests) {
        if (argc == 2 && std::string_view(argv[1]) != t.name) continue;
        ++count;
        failures += !run_test(t);
    }
    if (!count) { std::cerr << "Unknown test; use --list\n"; return 2; }
    std::cout << "Summary: " << count - failures << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
