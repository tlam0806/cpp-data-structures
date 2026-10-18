#pragma once
#include<functional>
#include<cstddef>
#include<cassert>
#include<algorithm>
#include<memory>
#include<new>
#include<limits>
#include<stdexcept>
#include<type_traits>
#include<utility>
#include<stdexcept>

namespace lam {
template<class Key, class Value, class Hash>
concept RobinHoodCompatible =
    std::is_nothrow_move_constructible_v<std::pair<Key, Value>> &&
    std::is_nothrow_swappable_v<std::pair<Key, Value>> &&
    std::is_nothrow_invocable_r_v<std::size_t, const Hash&, const Key&> &&
    std::is_nothrow_swappable_v<Hash> &&
    requires(const Key& a, const Key& b) {
        { static_cast<bool>(a == b) } noexcept;
    };

template<
    typename Key, 
    typename Value,
    typename Hash = std::hash<Key>
>
    requires RobinHoodCompatible<Key, Value, Hash>
class RobinHood {
    using Element = std::pair<Key, Value>;
    
public : 
    template<bool IsConst>
    class basic_iterator {
        friend class RobinHood;
        template<bool> friend class basic_iterator;
        using Map = std::conditional_t<IsConst, const RobinHood, RobinHood>;

        using reference = std::pair<const Key&,
            std::conditional_t<IsConst, const Value&, Value&>>;

        struct arrow_proxy {
            reference ref;

            const reference* operator->() {
                return &ref;
            }
        };

        public : 
            basic_iterator(Map* map, std::size_t index) : 
                map_{map},
                index_{index}
            {}

            template<bool OtherConst>
                requires (IsConst && !OtherConst)
            basic_iterator(const basic_iterator<OtherConst>& other) :
                map_{other.map_}, index_{other.index_} {}

            reference operator*() const {
                auto& ele = map_->elements_[index_];
                return reference{ele.first, ele.second};
            }

            arrow_proxy operator->() const {
                auto& ele = map_->elements_[index_];
                return arrow_proxy{{ele.first, ele.second}};
            }

            basic_iterator& operator++() {
                while (++index_ < map_->bucket_size_) {
                    if (map_->distances_[index_] != EMPTY_SENTINEL) {
                        return *this;
                    }
                }
                assert(index_ == map_->bucket_size_);
                return *this;
            }

            basic_iterator operator++(int) {
                basic_iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            template<bool OtherConst>
            bool operator ==(const basic_iterator<OtherConst>& other) const {
                return map_ == other.map_ && index_ == other.index_;
            }
        private : 
            Map* map_;
            std::size_t index_;
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    explicit RobinHood(std::size_t bucket_size = INITIAL_BUCKET_SIZE) :
        elements_{allocate_raw<Element>(bucket_size)},
        distances_{allocate_raw<std::size_t>(bucket_size)},
        bucket_size_{bucket_size},
        element_size_{0},
        hasher_{}
    {
        if (bucket_size == 0) {
            throw std::invalid_argument("bucket size must be greater than zero");
        }
        std::fill_n(distances_.get(), bucket_size_, EMPTY_SENTINEL);
    }

    RobinHood(const RobinHood& other) :
        RobinHood()
    {
        for (const Element& element : other) {
            insert(element);
        }
    }

    RobinHood& operator=(const RobinHood& other) {
        if (this == &other) {
            return *this;
        }
        erase_all();
        for (const Element& element : other) {
            insert(element);
        }
        return *this;
    }

    RobinHood(RobinHood&& other) : 
        RobinHood()
    {
        swap(other);
    }
    
    RobinHood& operator=(RobinHood&& other) {
        if (this == &other) {
            return *this;
        }
        erase_all();
        swap(other);
        return *this;
    }

    Value& operator[](const Key& key) {
        auto it = find(key);
        if (it != end()) {
            return it->second;
        }
        return insert({key, Value()})->second;
    }

    bool empty() const {
        return element_size_ == 0;
    }

    std::size_t size() const {
        return element_size_;
    }

    iterator begin() { return iterator(this, first_index()); }
    const_iterator begin() const { return const_iterator(this, first_index()); }
    const_iterator cbegin() const { return begin(); }

    iterator end() { return iterator(this, bucket_size_); }
    const_iterator end() const { return const_iterator(this, bucket_size_); }
    const_iterator cend() const { return end(); }

    iterator find(const Key& key) { return iterator(this, find_index(key)); }
    const_iterator find(const Key& key) const {
        return const_iterator(this, find_index(key));
    }

    iterator insert(const Element& element) {
        return insert_impl(element);
    }

    iterator insert(Element&& element) {
        return insert_impl(std::move(element));
    }   

    std::size_t erase(const Key& key) {
        iterator found = find(key);
        if (found == end()) {
            return 0;
        }
        std::size_t index = found.index_;
        assert(key == std::as_const(elements_[index].first));
        distances_[index] = EMPTY_SENTINEL;
        shift_backward(index);
        --element_size_;
        return 1;
    }

    iterator erase(const iterator& it) {
        const Key& key = it->first;
        iterator found = find(key);
        if (found == end()) {
            return end();
        }
        std::size_t index = found.index_;
        assert(key == std::as_const(elements_[index].first));
        distances_[index] = EMPTY_SENTINEL;
        shift_backward(index);
        --element_size_;
        while (index < bucket_size_ && distances_[index] == EMPTY_SENTINEL) ++index;
        return iterator(this, index);
    }

    ~RobinHood() {
        erase_all();
    }

private : 
    static constexpr std::size_t GROWTH_FACTOR = 2;
    static constexpr std::size_t INITIAL_BUCKET_SIZE = 8;
    static constexpr std::size_t EMPTY_SENTINEL = 0;
    static constexpr std::size_t STARTING_DISTANCE = 1;
    static constexpr std::size_t LOAD_NUMERATOR = 3;
    static constexpr std::size_t LOAD_DENOMINATOR = 4;

    // template<typename U>
    // U* allocate_raw(std::size_t count) const {
    //     return std::allocator<U>{}.allocate(count);
    // }

    template<typename T>
    struct raw_deleter {
        std::size_t count;

        void operator()(T* pointer) {
            std::allocator<T>{}.deallocate(pointer, count);
        }
    };

    template<typename T>
    using RawBuffer = std::unique_ptr<T[], raw_deleter<T>>;

    template<typename T> 
    static RawBuffer<T> allocate_raw(std::size_t count) {
        T* pointer = std::allocator<T>{}.allocate(count);
        raw_deleter<T> deleter{count};
        return RawBuffer<T>{
            pointer, deleter
        };
    }

    std::size_t first_index() const {
        std::size_t index = 0;
        while (index < bucket_size_ && distances_[index] == EMPTY_SENTINEL) ++index;
        return index;
    }

    std::size_t find_index(const Key& key) const {
        std::size_t index = bucket_index(key);
        std::size_t distance = STARTING_DISTANCE;
        while (distances_[index] != EMPTY_SENTINEL) {
            if (distances_[index] < distance) break;
            if (distances_[index] == distance &&
                key == std::as_const(elements_[index].first)) return index;
            index = increase_index(index);
            ++distance;
        }
        return bucket_size_;
    }

    std::size_t bucket_index(const Key& key) const {
        return hasher_(key) % bucket_size_;
    }

    std::size_t increase_index(std::size_t index) const {
        if (index == bucket_size_ - 1) {
            return 0;
        }
        return index + 1;
    }

    template<typename U>
    iterator insert_impl(U&& element) {
        if (needs_rehash()) {
            rehash();
            return insert(std::forward<U>(element));
        }
        Element incoming{std::forward<U>(element)};
        const Key& key = incoming.first;
        std::size_t cur_distance = STARTING_DISTANCE;
        std::size_t inserted = bucket_size_;
        std::size_t index = bucket_index(key);

        while (distances_[index] != EMPTY_SENTINEL) {
            if (distances_[index] == cur_distance && key == std::as_const(elements_[index].first)) {
                return iterator(this, index);
            }
            if (distances_[index] < cur_distance) {
                if (inserted == bucket_size_) inserted = index;
                std::swap(elements_[index], incoming);
                std::swap(distances_[index], cur_distance);
            }
            ++cur_distance;
            index = increase_index(index);
        }
        assert(distances_[index] == EMPTY_SENTINEL);
        if (inserted == bucket_size_) {
            inserted = index;
        }
        std::construct_at(
            elements_.get() + index,
            std::move(incoming)
        );  
        distances_[index] = cur_distance;
        ++element_size_;
        return iterator(this, inserted);
    }

    void erase_all() {
        for (std::size_t i = 0; i < bucket_size_; ++i) {
            if (distances_[i] != EMPTY_SENTINEL) {
                std::destroy_at(elements_.get() + i);
                distances_[i] = EMPTY_SENTINEL;
            }
        }
        element_size_ = 0;
    }

    bool needs_rehash() const noexcept {
        const auto quotient = bucket_size_ / LOAD_DENOMINATOR;
        const auto remainder = bucket_size_ % LOAD_DENOMINATOR;

        const auto max_elements =
            quotient * LOAD_NUMERATOR +
            remainder * LOAD_NUMERATOR / LOAD_DENOMINATOR;

        return element_size_ >= max_elements;
    }

    void rehash() {
        if (bucket_size_ > std::numeric_limits<std::size_t>::max() / GROWTH_FACTOR) {
            throw std::length_error("RobinHood capacity overflow");
        }
        std::size_t new_bucket_size = bucket_size_ * GROWTH_FACTOR;
        RobinHood tmp(new_bucket_size);
        for (std::size_t i = 0; i < bucket_size_; ++i) {
            if (distances_[i] != EMPTY_SENTINEL) {
                tmp.insert(std::move(elements_[i]));
            }
        }
        swap(tmp);
    }

    void rehash(std::size_t new_bucket_size) {
        RobinHood tmp(new_bucket_size);
        for (std::size_t i = 0; i < bucket_size_; ++i) {
            if (distances_[i] != EMPTY_SENTINEL) {
                tmp.insert(std::move(elements_[i]));
            }
        }
        swap(tmp);
    }
    
    void swap(RobinHood& other) {
        std::swap(elements_, other.elements_);
        std::swap(distances_, other.distances_);
        std::swap(bucket_size_, other.bucket_size_);
        std::swap(element_size_, other.element_size_);
        using std::swap;
        swap(hasher_, other.hasher_);
    }

    void shift_backward(std::size_t index) { 
        while (true) {
            std::size_t next_index = increase_index(index);
            if (distances_[next_index] <= STARTING_DISTANCE) { 
                break;
            }
            distances_[index] = distances_[next_index] - 1;
            std::swap(elements_[index], elements_[next_index]);
            index = next_index;
        }
        std::destroy_at(elements_.get() + index);
        distances_[index] = EMPTY_SENTINEL;
    }

    RawBuffer<Element> elements_;
    RawBuffer<std::size_t> distances_;
    std::size_t bucket_size_;
    std::size_t element_size_;
    Hash hasher_;
};

} // namespace lam
