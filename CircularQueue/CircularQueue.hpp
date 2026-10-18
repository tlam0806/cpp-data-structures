#pragma once
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
#include <limits>

namespace lam {
template<typename T>
class CircularQueue {
public : 
    template<bool IsConst>
    class basic_iterator {
        template<bool> friend class basic_iterator;
    public:
        using pointer = std::conditional_t<IsConst, const T*, T*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;

        basic_iterator(pointer data, std::size_t index, std::size_t capacity) :
            data_{data}, index_{index}, capacity_{capacity} {}

        template<bool OtherConst>
            requires (IsConst && !OtherConst)
        basic_iterator(const basic_iterator<OtherConst>& other) :
            data_{other.data_}, index_{other.index_}, capacity_{other.capacity_} {}

        reference operator*() const {
            return data_[index_ % capacity_];
        }

        basic_iterator& operator++() {
            ++index_;
            return *this;
        }

        template<bool OtherConst>
        bool operator==(const basic_iterator<OtherConst>& other) const {
            return data_ == other.data_ && index_ == other.index_ &&
                   capacity_ == other.capacity_;
        }

    private:
        pointer data_;
        std::size_t index_;
        std::size_t capacity_;
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    CircularQueue() :
        capacity_{1},
        size_{0},
        head_{0}, // last occupied
        data_{allocate_raw(1)}
    {
        
    }

    CircularQueue(const CircularQueue& other) : 
        CircularQueue() 
    {
        for (const auto& element : other) {
            push_back(element);
        }
    }

    CircularQueue(CircularQueue&& other) : 
        capacity_{std::exchange(other.capacity_, 1)},
        size_{std::exchange(other.size_, 0)},
        head_{std::exchange(other.head_, 0)},
        data_{std::exchange(other.data_, allocate_raw(1))}
    {

    }

    CircularQueue& operator=(CircularQueue other) {
        swap(other);
        return *this;
    }

    void swap(CircularQueue& other) {
        std::swap(capacity_, other.capacity_);
        std::swap(size_, other.size_);
        std::swap(head_, other.head_);
        std::swap(data_, other.data_);
    }

    ~CircularQueue() {
        for (auto& element : *this) {
            std::destroy_at(std::addressof(element));
        }
        deallocate_raw(data_, capacity_);
    }

    iterator begin() {
        return iterator(data_, head_, capacity_);
    }

    iterator end() {
        return iterator(data_, head_ + size_, capacity_);
    }

    const_iterator begin() const {
        return const_iterator(data_, head_, capacity_);
    }

    const_iterator end() const {
        return const_iterator(data_, head_ + size_, capacity_);
    }

    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    bool empty() const {
        return size_ == 0;
    }

    std::size_t size() const {
        return size_;
    }

    T& operator[](std::size_t index) {
        return data_[(head_ + index) % capacity_];
    }

    void push_front(const T& element) {
        emplace_front(element);
    }

    void push_front(T&& element) {
        emplace_front(std::move(element));
    }

    void push_back(const T& element) {
        emplace_back(element);
    }

    void push_back(T&& element) {
        emplace_back(std::move(element));
    }

    template<typename... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == capacity_) {
            if (capacity_ > max_count / 2) {
                throw std::bad_array_new_length{};
            }
            T tmp = T(std::forward<Args>(args)...);
            reserve(capacity_ * 2);
            auto returned = std::construct_at(
                data_ + (head_ + size_) % capacity_, 
                std::move_if_noexcept(tmp)
            );
            ++size_;
            return *returned;
        } else {
            auto returned = std::construct_at(
                data_ + (head_ + size_) % capacity_,
                T(std::forward<Args>(args)...)
            );
            ++size_;
            return *returned;
        }
    }

    template<typename... Args>
    T& emplace_front(Args&&... args) {
        if (size_ == capacity_) {
            if (capacity_ > max_count / 2) {
                throw std::bad_array_new_length{};
            }
            T tmp = T(std::forward<Args>(args)...);
            reserve(capacity_ * 2);
            const std::size_t new_head = (head_ - 1 + capacity_) % capacity_;
            auto returned = std::construct_at(
                data_ + new_head, 
                std::move_if_noexcept(tmp)
            );
            head_ = new_head;
            ++size_;
            return *returned;
        } else {
            const std::size_t new_head = (head_ - 1 + capacity_) % capacity_;
            auto returned = std::construct_at(
                data_ + new_head,
                std::forward<Args>(args)...
            );
            ++size_;
            head_ = new_head;
            return *returned;
        }
    }

    T& back() noexcept {
        // std::cout << head_ << " " << size_ << " " << capacity_ << "\n";
        return data_[(head_ + size_ - 1 + capacity_) % capacity_];
    }
    
    T& front() noexcept {
        return data_[head_];
    }

    const T& back() const noexcept {
        // std::cout << head_ << " " << size_ << " " << capacity_ << "\n";
        return data_[(head_ + size_ - 1 + capacity_) % capacity_];
    }
    
    const T& front() const noexcept {
        return data_[head_];
    }

    void reserve(std::size_t new_capacity) {
        if (new_capacity <= capacity_) {
            return;
        }
        T* new_data = allocate_raw(new_capacity);
        std::size_t constructed = 0;
        try {
            for (std::size_t i = 0; i < size_; ++i) {
                T* old_addr = data_ + (head_ + i) % capacity_;
                std::construct_at(
                    new_data + i,
                    std::move_if_noexcept(*old_addr)
                );
                ++constructed;
            }
            for (std::size_t i = 0; i < size_; ++i) {
                T* old_addr = data_ + (head_ + i) % capacity_;
                std::destroy_at(
                    old_addr
                );
            }
        } catch(...) {
            for (std::size_t i = 0; i < constructed; ++i) {
                T* old_addr = data_ + (head_ + i) % capacity_;
                std::destroy_at(new_data + i);
            }
            deallocate_raw(new_data, new_capacity);
            throw;
        }
        deallocate_raw(data_, capacity_);
        data_ = new_data;
        capacity_ = new_capacity;
        head_ = 0;
    }

private : 
    static constexpr auto max_count = std::numeric_limits<std::size_t>::max() / sizeof(T);
    std::size_t capacity_ = 0;
    std::size_t size_;
    std::size_t head_;
    T* data_;

    static T* allocate_raw(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length{};
        }
        if (count == 0) {
            return nullptr;
        }
        return std::allocator<T>{}.allocate(count);
    }

    static void deallocate_raw(T* data, std::size_t count) noexcept {
        if (data) {
            std::allocator<T>{}.deallocate(data, count);
        }
    }


};

} // namespace lam
