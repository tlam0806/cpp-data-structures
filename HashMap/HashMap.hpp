#pragma once
#include<functional>
#include<vector>
#include<cassert>
#include<memory>
#include<algorithm>
#include<utility>

namespace lam {
template<
    typename Key, 
    typename Value,
    typename Hash = std::hash<Key>
> 
    requires (
        std::is_nothrow_invocable_r_v<std::size_t, const Hash&, const Key&>
    )
class HashMap {
    using Element = std::pair<const Key, Value>;
    struct Node;
    struct NodeBase;
public :
    template<bool IsConst>
    class BasicIterator {
        template<bool>
        friend class BasicIterator;
        friend class HashMap;
        using NodeBasePointer = std::conditional_t<IsConst, const NodeBase*, NodeBase*>;
        using NodePointer = std::conditional_t<IsConst, const Node*, Node*>;
        using reference = std::conditional_t<IsConst, const Element, Element>;
        public :
            explicit BasicIterator(NodeBasePointer node) noexcept :
                node_{node}
            {}

            template<bool OtherConst>
                requires(IsConst && !OtherConst)
            BasicIterator(const BasicIterator<OtherConst>& other) noexcept :
                node_{other.node_}
            {}

            template<bool OtherConst> 
            bool operator==(const BasicIterator<OtherConst>& other) const noexcept {
                return node_ == other.node_;
            }

            BasicIterator& operator++() noexcept {
                node_ = node_->next_;
                return *this;
            }

            reference& operator*() const noexcept {
                return static_cast<NodePointer>(node_)->element_;
            }

            reference* operator->() const noexcept {
                return std::addressof(static_cast<NodePointer>(node_)->element_);
            }
        private : 
            NodeBasePointer node_;
    };

    using iterator = BasicIterator<false>;
    using const_iterator = BasicIterator<true>;

    explicit HashMap(std::size_t bucket_count = initial_bucket_capacity_) :
        dummy_{std::make_unique<NodeBase>(nullptr)},
        buckets_(bucket_count),
        bucket_capacity_{bucket_count},
        element_count_{0},
        hasher_{}
    {
        assert(bucket_count > 0);
    }

    HashMap(const HashMap& other) :
        HashMap(other.bucket_capacity_)
    {
        for (const auto &element : other) {
            insert(element);
        }
    }

    HashMap(HashMap&& other) :
        HashMap()
    {
        swap(other);
    }

    HashMap& operator=(HashMap other) noexcept {
        swap(other);
        return *this;
    }

    void clear() noexcept {
        NodeBase* cur = dummy_->next_;
        while (cur != nullptr) {
            NodeBase* next = cur->next_;
            delete cur;
            cur = next;
        }
        dummy_->next_ = nullptr;
        element_count_ = 0;
        for (std::size_t i = 0; i < bucket_capacity_; ++i) {
            buckets_[i].before_first_ = buckets_[i].last_ = nullptr;
        }
    }

    ~HashMap() noexcept {
        clear();
    }

    void swap(HashMap& other) {
        std::swap(dummy_, other.dummy_);
        std::swap(buckets_, other.buckets_);
        std::swap(bucket_capacity_, other.bucket_capacity_);
        std::swap(element_count_, other.element_count_);
        std::swap(hasher_, other.hasher_);
    }

    bool empty() const noexcept {
        return element_count_ == 0;
    }

    std::size_t size() const noexcept {
        return element_count_;
    }

    iterator begin() noexcept {
        return iterator(dummy_->next_);
    }
    const_iterator begin() const noexcept {
        return const_iterator(dummy_->next_);
    }
    const_iterator cbegin() const noexcept {
        return const_iterator(dummy_->next_);
    }

    iterator end() noexcept {
        return iterator(nullptr);
    }
    const_iterator end() const noexcept {
        return const_iterator(nullptr);
    }
    const_iterator cend() const noexcept {
        return const_iterator(nullptr);
    }


    Value& operator[](const Key& key) {
        return subscript_impl(key);
    }

    Value& operator[](Key&& key) {
        return subscript_impl(std::move(key));
    }


    iterator insert(const Element& element) {
        return insert_impl(element);
    }

    iterator insert(Element&& element) {
        return insert_impl(std::move(element));
    }

    iterator find(const Key& key) { 
        NodeBase* found = find_impl(get_bucket_index(key), key);
        return iterator(found);
    }
    const_iterator find(const Key& key) const {
        NodeBase* found = find_impl(get_bucket_index(key), key);
        return const_iterator(found);
    }

    std::size_t erase(const Key& key) {
        std::size_t bucket_index = get_bucket_index(key);
        NodeBase* prev_found = find_prev_impl(bucket_index, key);
        if (prev_found == nullptr) {
            return 0;
        }
        Bucket& bucket = buckets_[bucket_index];
        NodeBase* found = prev_found->next_;
        NodeBase* next_found = found->next_;
        assert(static_cast<Node*>(found)->element_.first == key);
        prev_found->next_ = next_found;
        if (found == bucket.last_) {
            bucket.last_ = prev_found;
            if (next_found != nullptr) {
                std::size_t next_bucket_index = get_bucket_index(static_cast<Node*>(next_found)->element_.first);
                assert(next_bucket_index != bucket_index);
                buckets_[next_bucket_index].before_first_ = prev_found;
            }
        }
        if (bucket.last_ == bucket.before_first_) {
            bucket.before_first_ = bucket.last_ = nullptr;
        }
        delete found;
        --element_count_;
        return 1;
    }

    iterator erase(const iterator& it) {
        const Key& key = it->first;
        std::size_t bucket_index = get_bucket_index(key);
        NodeBase* prev_found = find_prev_impl(bucket_index, key);
        if (prev_found == nullptr) {
            return end();
        }
        Bucket& bucket = buckets_[bucket_index];
        NodeBase* found = prev_found->next_;
        assert(found == it.node_);
        NodeBase* next_found = found->next_;
        assert(static_cast<Node*>(found)->element_.first == key);
        prev_found->next_ = next_found;
        if (found == bucket.last_) {
            bucket.last_ = prev_found;
            if (next_found != nullptr) {
                std::size_t next_bucket_index = get_bucket_index(static_cast<Node*>(next_found)->element_.first);
                assert(next_bucket_index != bucket_index);
                buckets_[next_bucket_index].before_first_ = prev_found;
            }
        }
        if (bucket.last_ == bucket.before_first_) {
            bucket.before_first_ = bucket.last_ = nullptr;
        }
        delete found;
        --element_count_;
        return iterator(next_found);
    }

    bool need_rehash() {
        return (size() + 1 > static_cast<std::size_t>(bucket_capacity_ * max_load_factor_));
    }

    void rehash(std::size_t count) {
        std::size_t new_capacity = std::max(count, static_cast<std::size_t>(ceil((size() + 1) / max_load_factor_)));
        if (bucket_capacity_ >= count) {
            return;
        }
        HashMap other(new_capacity);
        NodeBase* cur = dummy_->next_; 
        while (cur != nullptr) {
            NodeBase* next = cur->next_;
            other.insert(cur);
            cur = next;
        }
        dummy_->next_ = nullptr;
        swap(other);
    }
    
private : 
    struct NodeBase {
        NodeBase* next_;

        explicit NodeBase(NodeBase* next) noexcept : next_{next}
        {}

        virtual ~NodeBase() = default;
    };
    
    struct Node : NodeBase {
        Element element_;

        template<typename ElementInput>
        Node(NodeBase* next, ElementInput&& element) noexcept : 
            NodeBase(next),
            element_{std::forward<ElementInput>(element)}
        {}
    };

    struct Bucket {
        NodeBase* before_first_{nullptr};
        NodeBase* last_{nullptr};
    };

    std::size_t get_bucket_index(const Key& key) const {
        return hasher_(key) % bucket_capacity_;
    }

    NodeBase* find_impl(std::size_t bucket_index, const Key& key) const {
        const Bucket& bucket = buckets_[bucket_index];
        if (bucket.last_ == nullptr) {
            return nullptr;
        }
        assert(bucket.last_ != nullptr);
        NodeBase* cur = bucket.before_first_;
        while (cur != bucket.last_) {
            cur = cur->next_;
            if (static_cast<Node*>(cur)->element_.first == key) {
                return cur;
            }
        }
        return nullptr;
    }

    NodeBase* find_prev_impl(std::size_t bucket_index, const Key& key) {
        Bucket& bucket = buckets_[bucket_index];
        if (bucket.last_ == nullptr) {
            return nullptr;
        }
        NodeBase* cur = bucket.before_first_;
        while (cur != bucket.last_) {
            NodeBase* next = cur->next_;
            assert(next != nullptr);
            if (static_cast<Node*>(next)->element_.first == key) {
                return static_cast<NodeBase*>(cur);
            }
            cur = next;
        }
        return nullptr;
    }

    template<typename ElementInput>
    iterator insert_impl(ElementInput&& element) {
        if (need_rehash()) {
            rehash(bucket_capacity_ * 2);
        }
        const Key& key = element.first;
        std::size_t bucket_index = get_bucket_index(key);
        Bucket& bucket = buckets_[bucket_index];
        if (bucket.last_ == nullptr) { // no bucket yet
            NodeBase* node = new Node(dummy_->next_, std::forward<ElementInput>(element));
            bucket.before_first_ = dummy_.get();
            bucket.last_ = node;

            if (dummy_->next_ != nullptr) {
                NodeBase* next_node = dummy_->next_;
                std::size_t next_bucket_index = get_bucket_index(static_cast<Node*>(next_node)->element_.first);
                assert(next_bucket_index != bucket_index);
                assert(buckets_[next_bucket_index].before_first_ == dummy_.get());
                buckets_[next_bucket_index].before_first_ = node;
            }
            dummy_->next_ = node;
            ++element_count_;
            return iterator(node);
        }
        NodeBase* found = find_impl(bucket_index, key);
        if (found != nullptr) {
            static_cast<Node*>(found)->element_.second = std::forward<ElementInput>(element).second;
            return iterator(found);
        } else {
            NodeBase* node = new Node(bucket.before_first_->next_, std::forward<ElementInput>(element));
            bucket.before_first_->next_ = node;
            ++element_count_;
            return iterator(node);
        }
    }

    void insert(NodeBase* node) {
        assert(!need_rehash());
        const Key& key = static_cast<Node*>(node)->element_.first;
        std::size_t bucket_index = get_bucket_index(key);
        Bucket& bucket = buckets_[bucket_index];
        if (bucket.last_ == nullptr) { // no bucket yet
            // NodeBase* node = new Node(dummy_->next_, std::forward<ElementInput>(element));
            node->next_ = dummy_->next_;
            bucket.before_first_ = dummy_.get();
            bucket.last_ = node;

            if (dummy_->next_ != nullptr) {
                NodeBase* next_node = dummy_->next_;
                std::size_t next_bucket_index = get_bucket_index(static_cast<Node*>(next_node)->element_.first);
                assert(next_bucket_index != bucket_index);
                assert(buckets_[next_bucket_index].before_first_ == dummy_.get());
                buckets_[next_bucket_index].before_first_ = node;
            }
            dummy_->next_ = node;
            ++element_count_;
            return;
        }
        assert(find_impl(bucket_index, key) == nullptr);
        node->next_ = bucket.before_first_->next_;
        // NodeBase* node = new Node(bucket.before_first_->next_, std::forward<ElementInput>(element));
        bucket.before_first_->next_ = node;
        ++element_count_;
    }

    template<typename KeyInput>
    Value& subscript_impl(KeyInput&& key) {
        auto it = find(key);
        if (it != end()) {
            return it->second;
        }
        return insert({std::forward<KeyInput>(key), Value{}})->second;
    }

    float load_factor() const noexcept {
        return element_count_ / bucket_capacity_;
    }

    static constexpr size_t growth_factor_ = 2;
    static constexpr size_t initial_bucket_capacity_ = 8;
    static constexpr float max_load_factor_ = 1.7f;

    std::unique_ptr<NodeBase> dummy_;
    std::vector<Bucket> buckets_;
    std::size_t bucket_capacity_;
    std::size_t element_count_;
    Hash hasher_;
};

} // namespace lam
