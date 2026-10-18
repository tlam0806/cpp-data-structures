#pragma once

#include <cstddef>
#include <new>
#include <utility>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace lam {
template <class T>
class Vector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    

    // Special member functions
    Vector() noexcept
        : data_{nullptr}, size_{0}, capacity_{0} {
    }
    
    Vector(std::initializer_list<T> values)  : 
        data_{allocate_raw(values.size())}, 
        size_{0},
        capacity_{values.size()} {
        try {
            for (const T& value : values) {
                push_back(value);
            }
        } catch (...) {
            clear();
            deallocate_raw(data_);
            data_ = nullptr;
            capacity_ = 0;
            throw;
        }
    }

    explicit Vector(size_type count) 
        : data_{nullptr}, size_{0}, capacity_{0} {
        try {
            resize(count);
        } catch (...) {
            deallocate_raw(data_);
            throw;
        }
    }

    Vector(size_type count, const T& value) 
        : data_{nullptr}, size_{0}, capacity_{0} {
        try {
            resize(count, value);
        } catch (...) {
            deallocate_raw(data_);
            throw;
        }
    }

    Vector(const Vector& other) :
        data_(allocate_raw(other.size_)), 
        size_{0}, 
        capacity_{other.size_}
    {
        try {
            for (; size_ < other.size_; ++size_) {
                new (data_ + size_) T(other[size_]);
            }
        } catch (...) {
            for (std::size_t i = 0; i < size_; i++) {
                data_[i].~T();
            }
            deallocate_raw(data_);
            throw;
        }
    } 

    Vector& operator=(const Vector& other) {
        Vector tmp(other);
        swap(tmp);
        return *this;
    }

    Vector(Vector&& other) noexcept : 
        data_{std::exchange(other.data_, nullptr)},
        size_{std::exchange(other.size_, 0)},
        capacity_(std::exchange(other.capacity_, 0)) {
    }

    Vector& operator=(Vector&& other) noexcept {
        Vector tmp(std::move(other));
        swap(tmp);
        return *this;
    }

    void swap(Vector& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
    }

    ~Vector() noexcept {
        clear();
        deallocate_raw(data_);
    }

    // Element access
    T& at(size_type index) {
        if (index >= size_) {
            throw std::out_of_range("Vector::at: index out of range");
        }
        return data_[index];
    }

    const T& at(size_type index) const {
        if (index >= size_) {
            throw std::out_of_range("Vector::at: index out of range");
        }
        return data_[index];
    }

    T& operator[](size_type index) noexcept {
        return data_[index];
    }

    const T& operator[](size_type index) const noexcept {
        return data_[index];
    }

    T& front() noexcept {
        return data_[0];
    }

    const T& front() const noexcept {
        return data_[0];
    }

    T& back() noexcept {
        return data_[size_ - 1];
    }

    const T& back() const noexcept {
        return data_[size_ - 1];
    }

    T* data() noexcept {
        return data_;
    }

    const T* data() const noexcept {
        return data_;
    }

    // Iterators
    T* begin() noexcept {
        return data_;
    }

    const T* begin() const noexcept {
        return data_;
    }

    T* end() noexcept {
        if (data_ == nullptr) {
            return nullptr;
        }
        return data_ + size_;
    }

    const T* end() const noexcept {
        if (data_ == nullptr) {
            return nullptr;
        }
        return data_ + size_;
    }

    // Capacity
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] size_type size() const noexcept {
        return size_;
    }

    [[nodiscard]] size_type capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] size_type max_size() const noexcept {
        return max_size_value_;
    }

    void reserve(size_type new_capacity) {
        if (new_capacity <= capacity_) {
            return;
        }
        auto new_data = allocate_raw(new_capacity);
        {
            std::size_t i = 0;
            try {
                for (; i < size_; ++i) {
                    new (new_data + i) T(std::move_if_noexcept(data_[i]));
                }
            } catch (...) {
                for (std::size_t j = 0; j < i; ++j) {
                    new_data[j].~T();
                }
                deallocate_raw(new_data);
                throw;
            }
        }
        
        
        for (std::size_t i = 0; i < size_; i++) {
            data_[i].~T();
        }
        deallocate_raw(data_);
        data_ = new_data;
        capacity_ = new_capacity;
    }

    // Modifiers
    void clear() noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            data_[i].~T();
        }
        size_ = 0;
    }

    void resize(size_type new_size, const T& value) {
        if (new_size > size_) {
            T tmp(value);
            reserve(new_size);
            std::size_t i = size_;
            try {
                for (; i < new_size; ++i) {
                    new (data_ + i) T(tmp);
                }
                size_ = new_size;
            } catch (...) {
                for (std::size_t j = size_; j < i; ++j) {
                    data_[j].~T();
                }
                throw;
            }
            
        } else {
            while (size_ > new_size) {
                pop_back();
            }
        }
    }

    void resize(size_type new_size) {
        if (new_size >= size_) {
            reserve(new_size);
            std::size_t i = size_;
            try {
                for (; i < new_size; ++i) {
                    new (data_ + i) T();
                }
                size_ = new_size;
            } catch (...) {
                for (std::size_t j = size_; j < i; ++j) {
                    data_[j].~T();
                }
                throw;
            }
            
        } else {
            while (size_ > new_size) {
                pop_back();
            }
        }
    }

    void push_back(const T& element) {
        emplace_back(element);
    }

    void push_back(T&& element) {
        emplace_back(std::move(element));
    }
    
    template<class... Args> 
    T& emplace_back(Args&&... args) {
        if (size_ == capacity_) [[unlikely]] {
            T tmp(std::forward<Args>(args)...);
            reserve(next_capacity());
            new (data_ + size_) T(std::move_if_noexcept(tmp));
        } else {
            new (data_ + size_) T(std::forward<Args>(args)...);
        }
        return data_[size_++];
    }

    void pop_back() noexcept {
        data_[--size_].~T();
    }

    T* erase(const T* position) {
        size_type index = static_cast<size_type> (position - data_);
        for (size_t i = index; i + 1 < size_; ++i) {
            data_[i] = std::move(data_[i + 1]);
        }
        pop_back();
        return data_ + index;
    }

    T* erase(const T* first, const T* last) {
        if (first == last) {
            return const_cast<T*>(first);
        }
        size_type first_index = static_cast<size_type>(first - data_);
        size_type last_index = static_cast<size_type>(last - data_);
        for (; last_index < size_; ++last_index, ++first_index) {
            data_[first_index] = std::move(data_[last_index]);
        }
        for (size_type i = first_index; i < size_; ++i) {
            data_[i].~T();
        }
        size_ = first_index;
        return const_cast<T*>(first);
    }

    T* insert(const T* position, const T& value) {
        if (position == end()) {
            push_back(value);
            return &back();
        }
        size_type index = static_cast<size_type>(position - data_);
        T tmp(value);
        if (size_ == capacity_) {
            reserve(next_capacity());
        }
        new (data_ + size_) T(std::move(data_[size_ - 1]));
        // Track the new tail before any assignment can throw. On failure the
        // values may change, but all live elements remain owned/destructible.
        ++size_;
        for (size_type i = size_ - 2; i > index; --i) {
            data_[i] = std::move(data_[i - 1]);
        }
        data_[index] = std::move(tmp);
        return data_ + index;
    }

    T* insert(const T* position, T&& value) {
        if (position == end()) {
            push_back(std::move(value));
            return &back();
        }
        size_type index = static_cast<size_type>(position - data_);
        T tmp(std::move(value));
        if (size_ == capacity_) {
            reserve(next_capacity());
        }
        new (data_ + size_) T(std::move(data_[size_ - 1]));
        // Track the new tail before any assignment can throw. On failure the
        // values may change, but all live elements remain owned/destructible.
        ++size_;
        for (size_type i = size_ - 2; i > index; --i) {
            data_[i] = std::move(data_[i - 1]);
        }
        data_[index] = std::move(tmp);
        return data_ + index;
    }

    void shrink_to_fit() {
        if (size_ == capacity_) {
            return;
        }
        if (size_ == 0) {
            clear();
            deallocate_raw(data_);
            capacity_ = 0;
            data_ = nullptr;
            return;
        }
        std::size_t i = 0;
        auto new_data = allocate_raw(size_);

        try {
            for (; i < size_; ++i) {
                new (new_data + i) T(std::move_if_noexcept(data_[i]));
            }
            for (std::size_t j = 0; j < size_; ++j) {
                data_[j].~T();
            }
            deallocate_raw(data_);
            data_ = new_data;
            capacity_ = size_;
        } catch (...) {
            for (std::size_t j = 0; j < i; ++j) {
                new_data[j].~T();
            }
            deallocate_raw(new_data);
            throw;
        }
    }

private:
    static constexpr size_type max_size_value_ = std::numeric_limits<size_type>::max() / sizeof(T);
    T* data_;
    size_type size_;
    size_type capacity_;

    static T* allocate_raw(size_type count) {
        if (count == 0) {
            return nullptr;
        }
        if (count > max_size_value_) {
             throw std::length_error(
                "Vector capacity exceeds max_size"
            );
        }
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{alignof(T)}));
        } else {
            return static_cast<T*>(::operator new(count * sizeof(T)));
        }
    }

    static void deallocate_raw(T* data) noexcept {
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            ::operator delete(data, std::align_val_t{alignof(T)});
        } else {
            ::operator delete(data);
        }
    }

    size_type next_capacity() const {
        if (capacity_ == max_size_value_) {
            throw std::length_error("Vector capacity exceeds max_size");
        }
        if (capacity_ > max_size_value_ / 2) {
            return max_size_value_;
        }

        return std::max(capacity_ + 1, capacity_ * 2);
    }
};

} // namespace lam
