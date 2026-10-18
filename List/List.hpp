#pragma once
#include<cstddef>
#include<utility>
#include<cassert>
#include <iostream>

namespace lam {
template<typename T>
class List {
private : 
    struct NodeBase;
    struct Node;
public :
    template<bool IsConst>
    class basic_iterator {
        using NodeBasePointer = std::conditional_t<IsConst, const NodeBase*, NodeBase*>;
        using NodePointer = std::conditional_t<IsConst, const Node*, Node*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;
        template<bool> 
        friend class basic_iterator;
        friend class List;
        public : 
            explicit basic_iterator(NodeBasePointer node) noexcept : node_{node} {}

            template<bool OtherConst> 
                requires (IsConst && !OtherConst)
            basic_iterator(const basic_iterator<OtherConst>& other) noexcept :
                node_{other.node_}
            {}

            reference operator*() const noexcept {
                return static_cast<NodePointer>(node_)->value_;
            }

            basic_iterator& operator++() {
                node_ = node_->next_;
                return *this;
            }

            template<bool OtherConst>
            bool operator==(const basic_iterator<OtherConst>& other) const noexcept {
                return node_ == other.node_;
            }

        private : 
            NodeBasePointer node_;
    };
    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    List() :
        dummy_{new NodeBase(nullptr, nullptr)},
        begin_node_{dummy_},
        end_node_{dummy_},
        size_{0}
    {
    }

    List(const List& other) :
        List()
    {
        
        for (const auto& x : other) {
            push_back(x);
        }
    }

    List(List&& other) noexcept :
        List()
     {
        swap(other);
        
    }

    List& operator=(List other) noexcept {
        swap(other);
        return *this;
    }

    template<typename Container>
    List(const Container& container) :
        List()
    {
        for (const auto& element : container) {
            push_back(element);
        }
    }

    List(std::initializer_list<T> values) :
        List()
    {
        for(const T& value : values) {
            push_back(value);
        }
    }

    iterator begin() noexcept {
        return iterator{begin_node_};
    }
    const_iterator begin() const noexcept {
        return const_iterator{begin_node_};
    }
    const_iterator cbegin() const noexcept {
        return const_iterator{begin_node_};
    }

    iterator end() noexcept {
        return iterator{end_node_};
    }
    const_iterator end() const noexcept {
        return const_iterator{end_node_};
    }
    const_iterator cend() const noexcept {
        return const_iterator{end_node_};
    }

    bool empty() const noexcept {
        return size_ == 0;
    }

    std::size_t size() const noexcept {
        return size_;
    }

    T& front() noexcept {
        assert(!empty());
        return static_cast<Node*>(begin_node_)->value_;
    }

    T& back() noexcept {
        assert(!empty());
        return static_cast<Node*>(end_node_->previous_)->value_;
    }

    const T& front() const noexcept {
        assert(!empty());
        return static_cast<const Node*>(begin_node_)->value_;
    }

    const T& back() const noexcept {
        assert(!empty());
        return static_cast<const Node*>(end_node_->previous_)->value_;
    }

    iterator insert(const const_iterator& pos, const T& element) {
        NodeBase* current_node = const_cast<NodeBase*>(pos.node_);
        NodeBase* new_node = new Node(current_node->previous_, current_node, element);
        if (new_node->previous_ != nullptr) {
            new_node->previous_->next_ = new_node;
        } else {
            begin_node_ = new_node;
        }
        assert(new_node->next_ == current_node);
        new_node->next_->previous_ = new_node;
        ++size_;
        return iterator{new_node};
    }

    iterator insert(const const_iterator& pos, T&& element) {
        NodeBase* current_node = const_cast<NodeBase*>(pos.node_);
        NodeBase* new_node = new Node(current_node->previous_, current_node, std::move(element));
        if (new_node->previous_ != nullptr) {
            new_node->previous_->next_ = new_node;
        } else {
            begin_node_ = new_node;
        }
        assert(new_node->next_ == current_node);
        new_node->next_->previous_ = new_node;
        ++size_;
        return iterator{new_node};
    }

    void push_front(const T& element) {
        insert(begin(), element);
    }

    void push_front(T&& element) {
        insert(begin(), std::move(element));
    }

    void push_back(const T& element) {
        insert(end(), element);
    }

    void push_back(T&& element) {
        insert(end(), std::move(element));
    }

    iterator erase(const iterator& pos) noexcept {
        assert(pos != end());
        NodeBase* erased_node = pos.node_;

        if (erased_node->previous_ != nullptr) {
            erased_node->previous_->next_ = erased_node->next_;
        } else {
            assert(erased_node == begin_node_);
            begin_node_ = erased_node->next_;
        }

        erased_node->next_->previous_ = erased_node->previous_;
        NodeBase* tmp = erased_node->next_;
        delete erased_node;
        --size_;
        return iterator{tmp};
    }

    ~List() noexcept {
        reset();
        delete dummy_;
    }

private :
    struct NodeBase {
        NodeBase* previous_;
        NodeBase* next_;

        NodeBase(NodeBase* previous, NodeBase* next) :
        previous_{previous},
        next_{next}
        {}

        virtual ~NodeBase() = default;
    };

    struct Node : NodeBase {
        T value_;

        template<typename U>
        Node(NodeBase* previous, NodeBase* next, U&& value) :
            NodeBase(previous, next),
            value_(std::forward<U>(value))
        {}
    };

    void reset() noexcept {
        NodeBase* cur = begin_node_;
        while (cur != end_node_) {
            NodeBase* next = cur->next_;
            delete cur;
            cur = next;
        }
        begin_node_ = end_node_;
        dummy_->previous_ = dummy_->next_ = nullptr;
        size_ = 0;
    }

    void swap(List& other) noexcept {
        std::swap(begin_node_, other.begin_node_);
        std::swap(dummy_, other.dummy_);
        std::swap(end_node_, other.end_node_);
        std::swap(size_, other.size_);
    }

    NodeBase* dummy_;
    NodeBase* begin_node_;
    NodeBase* end_node_;
    std::size_t size_;

    
};

} // namespace lam
