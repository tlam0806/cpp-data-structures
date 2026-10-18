#pragma once
#include<utility>

namespace lam {
template<typename T>
struct default_deleter {
    void operator()(T* pointer) const noexcept {
        delete pointer;
    }
};

template<typename Element>
struct default_deleter<Element[]> {
    void operator()(Element* pointer) const noexcept {
        delete[] pointer;
    }
};

template<typename T, typename Deleter = default_deleter<T>>
    requires(
        std::is_nothrow_default_constructible_v<Deleter> &&
        std::is_nothrow_move_constructible_v<Deleter> &&
        std::is_nothrow_move_assignable_v<Deleter> &&
        std::is_nothrow_invocable_v<Deleter&, std::remove_extent_t<T>*>
    )
class unique_ptr {
    using Element = std::remove_extent_t<T>;
public : 
    explicit unique_ptr(Element* pointer) noexcept :
        pointer_{pointer}, 
        deleter_{}
    {}

    unique_ptr(Element* pointer, Deleter deleter) noexcept : 
        pointer_{pointer},
        deleter_{std::move(deleter)}
    {}

    unique_ptr(const unique_ptr& other) = delete;
    unique_ptr& operator=(const unique_ptr& other) = delete;

    unique_ptr(unique_ptr&& other) noexcept :
        pointer_{std::exchange(other.pointer_, nullptr)},
        deleter_{std::move(other.deleter_)}
    {}

    unique_ptr& operator=(unique_ptr&& other) noexcept {
        if (this != &other) {
            if (pointer_) {
                deleter_(pointer_);
            }
            pointer_ = std::exchange(other.pointer_, nullptr);
            deleter_ = std::move(other.deleter_);
        }
        return *this;
    }

    void swap(unique_ptr& other) noexcept {
        using std::swap;
        swap(pointer_, other.pointer_);
        swap(deleter_, other.deleter_);
    }

    Element* get() const noexcept {
        return pointer_;
    }

    explicit operator bool() const noexcept {
        return pointer_ != nullptr;
    }

    Element& operator*() const noexcept 
        requires(!std::is_array_v<T>)
    {
        return *pointer_;
    }

    Element& operator[](std::size_t index) const noexcept 
        requires(std::is_unbounded_array_v<T>)
    {
        return pointer_[index];
    }

    Element* operator->() const noexcept 
        requires(!std::is_array_v<T>) 
    {
        return pointer_;
    }


    Element* release() noexcept {
        return std::exchange(pointer_, nullptr);
    }

    void reset(Element* replacement = nullptr) noexcept {
        if (pointer_) {
            deleter_(pointer_);
        }
        pointer_ = replacement;
    }

    Deleter& get_deleter() noexcept {
        return deleter_;
    }

    const Deleter& get_deleter() const noexcept {
        return deleter_;
    }


    ~unique_ptr() noexcept {
        if (pointer_) {
            deleter_(pointer_);
        }
    }
private :
    Element* pointer_;
    [[no_unique_address]] Deleter deleter_;
};

template<typename T, typename... Args>
    requires(!std::is_array_v<T>)
unique_ptr<T> make_unique(Args&&... args) {
    return unique_ptr<T>{
        new T(std::forward<Args>(args)...)
    };
}

template<typename T>
    requires(std::is_unbounded_array_v<T>)
unique_ptr<T> make_unique(std::size_t count) {
    using Element = std::remove_extent_t<T>;
    return unique_ptr<T>{
        new Element[count]{}
    };
}

} // namespace lam
