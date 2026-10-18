#pragma once
#include<vector>
#include <cassert>

namespace lam {
template<
    typename T,
    std::size_t B = 2,
    typename Container = std::vector<T>,
    typename Compare = std::less<T>
>
class Heap {
    static_assert(B >= 2);
public :
    Heap(const Compare& compare) :
        container_{},
        compare_{compare}
    {

    }

    Heap() :
        container_{},
        compare_{}
    {

    }

    bool empty() const {
        return container_.empty();
    }

    std::size_t size() const {
        return container_.size();
    }

    
    template<typename... Args> 
    void emplace(Args&&... args) {
        container_.emplace_back(std::forward<Args>(args)...);
        bubble_up(size() - 1);
    }

    void push(const T& element) {
        emplace(element);
    }

    void push(T&& element) {
        emplace(std::move(element));
    }

    const T& top() const {
        assert(!empty());
        return container_.front();
    }

    void pop() {
        std::swap(container_.front(), container_.back());
        container_.pop_back();
        bubble_down();
    }

private : 

    void bubble_up(std::size_t index) {
        while (index) {
            std::size_t parent_index = (index - 1) / B;
            if (compare_(container_[index], container_[parent_index])) {
                return;
            } 
            std::swap(container_[index], container_[parent_index]);
            index = parent_index;
        }
    }

    void bubble_down() {
        std::size_t index = 0;
        std::size_t cached_size = size();
        while (true) {
            std::size_t max_index = index;
            for (std::size_t child_index = index * B + 1; child_index < std::min(cached_size, index * B + B + 1); ++child_index) {
                if (compare_(container_[max_index], container_[child_index])) {
                    max_index = child_index;
                }
            }
            if (index == max_index) {
                return;
            }
            std::swap(container_[index], container_[max_index]);
            index = max_index;
        }
    }

    Container container_;
    Compare compare_;
};

} // namespace lam
