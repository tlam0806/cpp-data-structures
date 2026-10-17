#pragma once
#include <cassert>
#include <cstddef>
#include <iterator>
#include <list>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>

template<typename Key, typename Value>
class LFUCache {
public : 
    explicit LFUCache(std::size_t capcacity) noexcept :
        buckets_{},
        map_{},
        capacity_{capcacity} 
    {}

    LFUCache(const LFUCache& other) = delete;
    LFUCache& operator=(const LFUCache& other) = delete;

    LFUCache(LFUCache&& other) noexcept :
        LFUCache(0)
    {
        swap(other);
    }

    LFUCache& operator=(LFUCache&& other) {
        swap(other);
        return *this;
    }

    void swap(LFUCache&other) {
        std::swap(buckets_, other.buckets_);
        std::swap(map_, other.map_);
        std::swap(capacity_, other.capacity_);
    }

    std::optional<Value> get(const Key& key) {
        auto map_it = map_.find(key);
        if (map_it == map_.end()) {
            return std::nullopt;
        }
        const auto &[bucket_it, list_it] = map_it->second;
        map_it->second = increase_frequency(bucket_it, list_it);
        return list_it->second;
    }

    template<typename KeyInput, typename ValueInput> 
    void put(KeyInput&& key, ValueInput&& value) {
        if (capacity_ == 0) {
            return;
        }
        auto map_it = map_.find(key);
        if (map_it == map_.end()) {
            try_envict();
            if (buckets_.empty() || buckets_.front().frequency_ > 1) {
                buckets_.push_front(Bucket{1});
            }
            auto [new_map_it, inserted] = map_.try_emplace(key);
            assert(inserted);
            try {
                buckets_.front().list_.emplace_back(new_map_it->first, std::forward<ValueInput>(value));
            } catch (...) {
                map_.erase(key);
                if (buckets_.front().list_.empty()) {
                    buckets_.pop_front();
                }
                throw;
            }
            new_map_it->second = make_pair(buckets_.begin(), std::prev(buckets_.front().list_.end()));
        } else {
            auto& [bucket_it, list_it] = map_it->second;
            tie(bucket_it, list_it) = increase_frequency(bucket_it, list_it);
            list_it->second = std::forward<ValueInput>(value);
        }
    }

private : 
    struct Bucket {
        std::list<std::pair<const Key&, Value> > list_;
        std::size_t frequency_;
        explicit Bucket(std::size_t frequency) : list_{}, frequency_{frequency} {} 
    };

    using ListIt = typename std::list<std::pair<const Key&, Value> >::iterator;
    using BucketIt = typename std::list<Bucket>::iterator;
    using MapIt = typename std::unordered_map<Key, ListIt>::iterator;

    std::pair<BucketIt, ListIt> increase_frequency(
        const BucketIt& bucket_it, const ListIt& list_it) {
        auto next_bucket_it = std::next(bucket_it);
        std::size_t frequency = bucket_it->frequency_;
        if (next_bucket_it == buckets_.end() || next_bucket_it->frequency_ != frequency + 1) {
            next_bucket_it = buckets_.insert(next_bucket_it, Bucket(frequency + 1));
        }
        next_bucket_it->list_.splice(next_bucket_it->list_.end(), bucket_it->list_, list_it);
        if (bucket_it->list_.empty()) {
            buckets_.erase(bucket_it);
        }
        return make_pair(next_bucket_it, std::prev(next_bucket_it->list_.end()));
    }

    void try_envict() {
        if (map_.size() < capacity_) return;
        auto bucket_it = buckets_.begin();
        assert(!bucket_it->list_.empty());
        auto &[key, value] = bucket_it->list_.front(); 
        map_.erase(key);
        bucket_it->list_.pop_front();
        if (bucket_it->list_.empty()) {
            buckets_.pop_front();
        }
    }

    std::list<Bucket> buckets_;
    std::unordered_map<Key, std::pair<BucketIt, ListIt>> map_;
    std::size_t capacity_;
};