// No supporting includes before this header: it must include its dependencies.
#include "LFUCache.hpp"
#include "LFUCache.hpp"

using lam::LFUCache;

int main() {
    LFUCache<int, int> cache(1);
    cache.put(1, 2);
    auto result = cache.get(1);
    return result && *result == 2 ? 0 : 1;
}
