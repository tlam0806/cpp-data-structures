#pragma once
#include <cassert>
#include <atomic>
#include <utility>
#include <memory>

namespace detail {
template<typename T>
struct default_deleter {
    void operator()(T* object) {
        delete object;
    }
};

template<typename T>
struct default_deleter<T[]> {
    void operator()(T* object) {
        delete[] object;
    }
};

struct ControlBlock {
    std::atomic<std::size_t> strong_counter{1};
    std::atomic<std::size_t> weak_counter{1}; 
    virtual void dispose() = 0;
    virtual ~ControlBlock() = default;
};

template<typename T, typename Deleter = detail::default_deleter<T>>
struct ControlBlockFor : ControlBlock {
    T* ptr;
    [[no_unique_address]] Deleter deleter{};
    explicit ControlBlockFor(T* ptr) :
        ptr(ptr)
    {}

    ControlBlockFor(T* ptr, Deleter deleter) : 
        ptr{ptr},
        deleter{std::move(deleter)}
    {}
    virtual void dispose() override {
        deleter(ptr);
    }
};

template<typename T>
struct InplaceControlBlock : ControlBlock {
    alignas(T) std::byte storage[sizeof(T)];

    template<typename... Args>
    explicit InplaceControlBlock(Args&&... args) {
        std::construct_at(get_storage(), std::forward<Args>(args)...);
    }

    T* get_storage() noexcept {
        return std::launder(reinterpret_cast<T*>(storage));
    }

    virtual void dispose() override {
        std::destroy_at(get_storage());
    }
};

};

template<typename T>
class shared_ptr {
    template<typename U>
    friend class shared_ptr;
    template<typename U>
    friend class weak_ptr;
    template<typename U, typename... Args>
    friend shared_ptr<U> make_shared(Args&&... args);
public : 
    shared_ptr() noexcept = default;

    shared_ptr(std::nullptr_t) noexcept {}

    template<typename U>
        requires std::is_convertible_v<U*, T*>
    explicit shared_ptr(U* ptr) {
        try {
            ctrl_ = new detail::ControlBlockFor<U>(ptr);
            ptr_ = ptr;
        } catch(...) {
            detail::default_deleter<U>{}(ptr);
            throw;
        }
    }

    template<typename U, typename Deleter>
        requires (
            std::is_convertible_v<U*, T*> &&
            std::is_invocable_v<Deleter&,U*>
        )
    explicit shared_ptr(U* ptr, Deleter&& deleter) {
        try {
            ctrl_ = new detail::ControlBlockFor<U, std::remove_cvref_t<Deleter>>(
                ptr,
                std::forward<Deleter>(deleter)
            );
            ptr_ = ptr;
        } catch(...) {
            deleter(ptr);
            throw;
        }
    }

    shared_ptr(const shared_ptr& other) noexcept : 
        shared_ptr(other.ptr_, other.ctrl_, std::true_type{})
    {}
    
    template<typename U>
        requires std::is_convertible_v<U*, T*>
    shared_ptr(const shared_ptr<U>& other) noexcept :
        shared_ptr(other.ptr_, other.ctrl_, std::true_type{})
    {}

    shared_ptr(shared_ptr&& other) noexcept :
        ptr_{std::exchange(other.ptr_, nullptr)},
        ctrl_{std::exchange(other.ctrl_, nullptr)}
    {}    

    template<typename U>
        requires std::is_convertible_v<U*, T*>
    shared_ptr(shared_ptr<U>&& other) noexcept : 
        ptr_{std::exchange(other.ptr_, nullptr)},
        ctrl_{std::exchange(other.ctrl_, nullptr)}
    {}

    shared_ptr& operator=(shared_ptr other) noexcept {
        swap(other);
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U*, T*>
    shared_ptr& operator=(shared_ptr<U> other) noexcept {
        shared_ptr<T>(std::move(other)).swap(*this);
        return *this;
    }

    void swap(shared_ptr& other) noexcept {
        std::swap(ptr_, other.ptr_);
        std::swap(ctrl_, other.ctrl_);
    }

    void reset() noexcept {
        shared_ptr<T>().swap(*this);
    }

    template<typename U>
        requires std::is_convertible_v<U*, T*>
    void reset(U* ptr) {
        shared_ptr(ptr).swap(*this);
    }

    template<typename U, typename Deleter>
        requires std::is_convertible_v<U*, T*> && std::is_invocable_v<Deleter&, U*>
    void reset(U* ptr, Deleter deleter) {
        shared_ptr(ptr, std::forward<Deleter>(deleter)).swap(*this);
    }

    T* get() const noexcept {
        return ptr_;
    }

    T& operator*() const noexcept {
        return *ptr_;
    }

    T* operator->() const noexcept {
        return ptr_;
    }

    std::size_t use_count() const noexcept {
        return ctrl_ ? ctrl_->strong_counter.load(std::memory_order_relaxed) : 0;
    }

    explicit operator bool() const noexcept {
        return get() != nullptr;
    }

    template<typename U>
    bool operator==(const shared_ptr<U>& b) const noexcept {
        return get() == b.get();
    }

    bool operator==(std::nullptr_t) const noexcept {
        return get() == nullptr;
    }

    ~shared_ptr() noexcept {
        if (ctrl_ == nullptr) {
            return;
        }
        if (ctrl_->strong_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // can be optimized using atomic fence, but TSAN dont understand this optimization 
            ctrl_->dispose();
            if (ctrl_->weak_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // can be optimized using atomic fence, but TSAN dont understand this optimization 
                delete ctrl_;
            }
        }
    }
private : 
    T* ptr_ = nullptr;
    detail::ControlBlock* ctrl_ = nullptr;

    template<bool increase>
    shared_ptr(T* ptr, detail::ControlBlock* ctrl, std::bool_constant<increase>) noexcept :
        ptr_{ptr},
        ctrl_{ctrl}
    {
        if constexpr (increase) {
            if (ctrl_) {
                ctrl_->strong_counter.fetch_add(1, std::memory_order_relaxed);
            }    
        }
    }

    shared_ptr(T* ptr, detail::InplaceControlBlock<T>* ctrl) noexcept :
        ptr_{ptr},
        ctrl_{ctrl}
    {}
};

template<typename T>
class weak_ptr {
    template<typename U>
    friend class weak_ptr;
public : 
    weak_ptr() noexcept = default;

    template<typename U>
        requires std::is_convertible_v<U*, T*>
    weak_ptr(const shared_ptr<U>& other) noexcept :
        weak_ptr<T>(other.ptr_, other.ctrl_)
    {}

    weak_ptr(const weak_ptr& other) noexcept :
        weak_ptr<T>(other.ptr_, other.ctrl_)
    {}

    template<typename U>
        requires std::is_convertible_v<U*, T*>
    weak_ptr(const weak_ptr<U>& other) noexcept :
        weak_ptr<T>(other.ptr_, other.ctrl_)
    {}

    weak_ptr(weak_ptr&& other) noexcept :
        ptr_{std::exchange(other.ptr_, nullptr)},
        ctrl_{std::exchange(other.ctrl_, nullptr)}
    {}

    template<typename U>
        requires std::is_convertible_v<U*, T*>
    weak_ptr(weak_ptr<U>&& other) noexcept :
        ptr_{std::exchange(other.ptr_, nullptr)},
        ctrl_{std::exchange(other.ctrl_, nullptr)}
    {}

    weak_ptr& operator=(weak_ptr other) noexcept {
        swap(other);
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U*, T*>
    weak_ptr& operator=(weak_ptr<U> other) noexcept {
        weak_ptr<T>(std::move(other)).swap(*this);
        return *this;
    }

    void swap(weak_ptr& other) noexcept {
        std::swap(ptr_, other.ptr_);
        std::swap(ctrl_, other.ctrl_);
    }
    
    shared_ptr<T> lock() const noexcept {
        if (ctrl_ == nullptr) {
            return shared_ptr<T>();
        }
        auto& strong = ctrl_->strong_counter;
        std::size_t cached_value = strong.load(std::memory_order_relaxed);
        while (cached_value > 0) {
            if (strong.compare_exchange_weak(cached_value, cached_value + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                return shared_ptr(ptr_, ctrl_, std::false_type{});
            } 
        }
        return shared_ptr<T>();
    }

    bool expired() const noexcept {
        return use_count() == 0;
    }

    std::size_t use_count() const noexcept {
        return ctrl_ ? ctrl_->strong_counter.load(std::memory_order_relaxed) : 0;
    }

    void reset() noexcept {
        weak_ptr().swap(*this);
    }

    ~weak_ptr() {
        if (ctrl_ == nullptr) {
            return;
        }
        if (ctrl_->weak_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // can be optimized using atomic fence, but TSAN dont understand this optimization 
            delete ctrl_;
        }
    }
private : 
    T* ptr_ = nullptr;
    detail::ControlBlock* ctrl_ = nullptr;

    weak_ptr(T* ptr, detail::ControlBlock* ctrl) noexcept : 
        ptr_{ptr},
        ctrl_{ctrl}
    {
        if (ctrl_) {
            ctrl_->weak_counter.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

template<typename T, typename... Args>
shared_ptr<T> make_shared(Args&&... args) {
    detail::InplaceControlBlock<T>* ctrl = new detail::InplaceControlBlock<T>(std::forward<Args>(args)...);
    return shared_ptr(ctrl->get_storage(), ctrl, std::false_type{});
}
