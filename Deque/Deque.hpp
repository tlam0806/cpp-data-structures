#pragma once

#include<algorithm>
#include<vector>
#include<cassert>

namespace lam {
inline constexpr std::size_t CACHE_SIZE = std::hardware_constructive_interference_size;

template<
    typename T,
    std::size_t BLOCK_SIZE = std::max(std::size_t{1}, CACHE_SIZE / alignof(T))
>
class Deque {
    struct Location;
public :
    template<bool IsConst>
    class BasicIterator {
        using DequePointer = std::conditional_t<IsConst, const Deque*, Deque*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;
        template<bool>
        friend class BasicIterator;
        public :
            BasicIterator(Location location, DequePointer deque, std::size_t logical_index) noexcept :
                location_{location},
                deque_{deque},
                logical_index_{logical_index}
            {}

            template<bool OtherConst>
                requires(IsConst && !OtherConst)
            BasicIterator(const BasicIterator<OtherConst>& other) noexcept :
                location_{other.location_},
                deque_{other.deque_},
                logical_index_{other.logical_index_}
            {}

            reference operator*() const noexcept {
                return deque_->subscript_impl(location_);
            }

            template<bool OtherConst>
            bool operator==(const BasicIterator<OtherConst>& other) const noexcept {
                return deque_ == other.deque_ && logical_index_ == other.logical_index_;
            }

            BasicIterator& operator++() noexcept {
                ++logical_index_;
                if (++location_.element_index_in_block_ == BLOCK_SIZE) {
                    location_.element_index_in_block_ = 0;
                    ++location_.block_index_;
                }
                return *this;
            }

        private :
            Location location_;
            DequePointer deque_;
            std::size_t logical_index_;
    };
    using iterator = BasicIterator<false>;
    using const_iterator = BasicIterator<true>;

    Deque() noexcept :
        blocks_{},
        block_capacity_{0},
        offset_{0},
        element_count_{0}
    {}

    Deque(const Deque& other) :
        Deque()
    {
        for (const auto& element : other) {
            push_back(element);
        }
    }

    Deque(Deque&& other) noexcept :
        Deque()
    {
        swap(other);
    }

    Deque& operator=(Deque other) noexcept {
        swap(other);
        return *this;
    }

    void swap(Deque& other) noexcept {
        std::swap(blocks_, other.blocks_);
        std::swap(block_capacity_, other.block_capacity_);
        std::swap(offset_, other.offset_);
        std::swap(element_count_, other.element_count_);
    }

    std::size_t size() const noexcept {
        return element_count_;
    }

    bool empty() const noexcept {
        return element_count_ == 0;
    }

    iterator begin() noexcept {
        return iterator{locate(0), this, 0};
    }
    const_iterator begin() const noexcept {
        return const_iterator{locate(0), this, 0};
    }
    const_iterator cbegin() const noexcept {
        return const_iterator{locate(0), this, 0};
    }

    iterator end() noexcept {
        return iterator{locate(element_count_), this, element_count_};
    }
    const_iterator end() const noexcept {
        return const_iterator{locate(element_count_), this, element_count_};
    }
    const_iterator cend() const noexcept {
        return const_iterator{locate(element_count_), this, element_count_};
    }

    T& operator[](std::size_t logical_index) noexcept {
        return subscript_impl(locate(logical_index));
    }

    const T& operator[](std::size_t logical_index) const noexcept {
        return subscript_impl(locate(logical_index));
    }

    T& front() noexcept {
        return (*this)[0];
    }
    const T& front() const noexcept {
        return (*this)[0];
    }

    T& back() noexcept {
        return (*this)[element_count_ - 1];
    }
    const T& back() const noexcept {
        return (*this)[element_count_ - 1];
    }

    template<typename... Args>
    T& emplace_back(Args&&... args) {
        if (element_count_ >= capped_logical_index()) {
            reserve_block(grow_capacity());
        }
        auto [block_index, element_index_in_block] = locate(element_count_);
        if (blocks_[block_index] == nullptr) {
            blocks_[block_index] = std::allocator<T>{}.allocate(BLOCK_SIZE);
        }
        auto returned = std::construct_at(
            blocks_[block_index] + element_index_in_block,
            std::forward<Args>(args)...
        );
        ++element_count_;
        return *returned;
    }

    template<typename... Args>
    T& emplace_front(Args&&... args) {
        if (offset_ == 0) {
            reserve_block(grow_capacity());
        }
        auto [block_index, element_index_in_block] = locate(NEGATIVE_ONE);
        if (blocks_[block_index] == nullptr) {
            blocks_[block_index] = std::allocator<T>{}.allocate(BLOCK_SIZE);
        }
        auto returned = std::construct_at(
            blocks_[block_index] + element_index_in_block,
            std::forward<Args>(args)...
        );
        ++element_count_;
        --offset_;
        return *returned;
    }

    void push_back(const T& element) {
        emplace_back(element);
    }
    void push_back(T&& element) {
        emplace_back(std::move(element));
    }

    void push_front(const T& element) {
        emplace_front(element);
    }
    void push_front(T&& element) {
        emplace_front(std::move(element));
    }

    void pop_back() noexcept {
        assert(!empty());
        std::destroy_at(std::addressof(back()));
        --element_count_;
    }

    void pop_front() noexcept {
        assert(!empty());
        std::destroy_at(std::addressof(front()));
        --element_count_;
        ++offset_;
    }

    ~Deque() noexcept {
        for (auto &element : *this) {
            std::destroy_at(std::addressof(element));
        }
        for (Block block : blocks_) if (block != nullptr) {
            std::allocator<T>{}.deallocate(block, BLOCK_SIZE);
        }
    }

private :
    struct Location {
        std::size_t block_index_;
        std::size_t element_index_in_block_;
    };

    Location locate(std::size_t logical_index) const noexcept {
        if (logical_index == NEGATIVE_ONE) {
            assert(offset_ != 0);
            std::size_t physical_index = offset_ - 1;
            std::size_t block_index = physical_index / BLOCK_SIZE;
            std::size_t element_index_in_block = physical_index % BLOCK_SIZE;
            return Location{
                block_index,
                element_index_in_block
            };
        } else {
            std::size_t physical_index = offset_ + logical_index;
            std::size_t block_index = physical_index / BLOCK_SIZE;
            std::size_t element_index_in_block = physical_index % BLOCK_SIZE;
            return Location{
                block_index,
                element_index_in_block
            };
        }
    }

    std::size_t grow_capacity() const noexcept {
        return std::max(std::size_t{2}, block_capacity_ * 2);
    }

    void reserve_block(std::size_t new_capacity) {
        std::vector<Block> new_blocks(new_capacity);
        std::size_t first_block = offset_ / BLOCK_SIZE;
        std::size_t end_block = offset_ + element_count_ == 0 ? 0 :  (offset_ + element_count_ - 1) / BLOCK_SIZE + 1;
        std::size_t used_blocks = end_block - first_block;
        std::size_t new_first_block = (new_capacity - used_blocks) / 2;

        for (std::size_t i = first_block, j = new_first_block; i < end_block; ++i, ++j) {
            assert(j < new_capacity);
            new_blocks[j] = blocks_[i];
        }
        for (std::size_t i = 0; i < first_block; ++i) {
            if (blocks_[i] != nullptr) {
                std::allocator<T>{}.deallocate(blocks_[i], BLOCK_SIZE);
            }
        }
        for (std::size_t i = end_block; i < block_capacity_; ++i) {
            if (blocks_[i] != nullptr) {
                std::allocator<T>{}.deallocate(blocks_[i], BLOCK_SIZE);
            }
        }
        blocks_ = std::move(new_blocks);
        block_capacity_ = new_capacity;
        //new offset
        assert(offset_ == first_block * BLOCK_SIZE + (offset_ % BLOCK_SIZE));
        offset_ = new_first_block * BLOCK_SIZE + offset_ % BLOCK_SIZE;
    }

    T& subscript_impl(Location location) noexcept {
        assert(blocks_[location.block_index_] != nullptr);
        return blocks_[location.block_index_][location.element_index_in_block_];
    }

    const T& subscript_impl(Location location) const noexcept {
        assert(blocks_[location.block_index_] != nullptr);
        return blocks_[location.block_index_][location.element_index_in_block_];
    }

    std::size_t capped_logical_index() const noexcept {
        return block_capacity_ * BLOCK_SIZE - offset_;
    }

    static constexpr std::size_t NEGATIVE_ONE = -1;

    using Block = T*;
    std::vector<Block> blocks_;
    std::size_t block_capacity_;
    std::size_t offset_;
    std::size_t element_count_;
};

} // namespace lam
