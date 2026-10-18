#include "Heap.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <random>
#include <vector>

using lam::Heap;

namespace {

using CustomHeap2 = Heap<int, 2>;
using CustomHeap4 = Heap<int, 4>;
using CustomHeap8 = Heap<int, 8>;
using StandardHeap = std::priority_queue<int>;

constexpr std::uint32_t random_seed = 0xC0FFEEU;

struct RandomInput {
    static std::vector<int> make(std::size_t count) {
        std::mt19937 random(random_seed);
        std::uniform_int_distribution<int> values(
            std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max());

        std::vector<int> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(values(random));
        }
        return result;
    }
};

struct AscendingInput {
    static std::vector<int> make(std::size_t count) {
        std::vector<int> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(static_cast<int>(index));
        }
        return result;
    }
};

struct DescendingInput {
    static std::vector<int> make(std::size_t count) {
        std::vector<int> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(static_cast<int>(count - index));
        }
        return result;
    }
};

struct DuplicateInput {
    static std::vector<int> make(std::size_t count) {
        std::mt19937 random(random_seed);
        std::uniform_int_distribution<int> values(-8, 8);

        std::vector<int> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(values(random));
        }
        return result;
    }
};

std::size_t input_size(const benchmark::State& state) {
    return static_cast<std::size_t>(state.range(0));
}

void record_work(benchmark::State& state, std::size_t operations_per_iteration) {
    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(operations_per_iteration));
    state.SetComplexityN(static_cast<std::int64_t>(operations_per_iteration));
}

template <typename Queue, typename Input>
void BM_Build(benchmark::State& state) {
    const std::vector<int> values = Input::make(input_size(state));
    std::optional<Queue> heap;

    for (auto _ : state) {
        heap.emplace();
        for (const int value : values) {
            heap->push(value);
        }

        int top = heap->top();
        benchmark::DoNotOptimize(top);
        benchmark::ClobberMemory();

        // Destruction is not part of building the heap.
        state.PauseTiming();
        heap.reset();
        state.ResumeTiming();
    }

    record_work(state, values.size());
}

template <typename Queue, typename Input>
void BM_Drain(benchmark::State& state) {
    const std::vector<int> values = Input::make(input_size(state));

    Queue original;
    for (const int value : values) {
        original.push(value);
    }

    std::optional<Queue> heap;
    for (auto _ : state) {
        // Copying creates the same initial heap for every iteration but is not
        // part of the pop benchmark.
        state.PauseTiming();
        heap.emplace(original);
        state.ResumeTiming();

        std::uint64_t checksum = 0;
        while (!heap->empty()) {
            checksum += static_cast<std::uint64_t>(heap->top());
            heap->pop();
        }
        benchmark::DoNotOptimize(checksum);

        state.PauseTiming();
        heap.reset();
        state.ResumeTiming();
    }

    record_work(state, values.size());
}

struct Operation {
    bool push;
    int value;
};

std::vector<Operation> make_mixed_operations(std::size_t count) {
    std::mt19937 random(random_seed);
    std::uniform_int_distribution<int> values(
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::max());
    std::bernoulli_distribution should_push(0.55);

    std::vector<Operation> operations;
    operations.reserve(count);

    std::size_t heap_size = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const bool push = heap_size == 0 || should_push(random);
        operations.push_back({push, values(random)});
        if (push) {
            ++heap_size;
        } else {
            --heap_size;
        }
    }
    return operations;
}

template <typename Queue>
void BM_Mixed(benchmark::State& state) {
    const std::vector<Operation> operations =
        make_mixed_operations(input_size(state));
    std::optional<Queue> heap;

    for (auto _ : state) {
        heap.emplace();
        std::uint64_t checksum = 0;

        for (const Operation& operation : operations) {
            if (operation.push) {
                heap->push(operation.value);
            } else {
                checksum += static_cast<std::uint64_t>(heap->top());
                heap->pop();
            }
        }
        benchmark::DoNotOptimize(checksum);
        benchmark::ClobberMemory();

        state.PauseTiming();
        heap.reset();
        state.ResumeTiming();
    }

    record_work(state, operations.size());
}

#define REGISTER_WITH_RANGE(function, queue, input)       \
    BENCHMARK_TEMPLATE(function, queue, input)            \
        ->RangeMultiplier(4)                              \
        ->Range(1 << 10, 1 << 20)                         \
        ->Complexity()

#define REGISTER_INPUT_BENCHMARKS(function, queue)                 \
    REGISTER_WITH_RANGE(function, queue, RandomInput);             \
    REGISTER_WITH_RANGE(function, queue, AscendingInput);          \
    REGISTER_WITH_RANGE(function, queue, DescendingInput);         \
    REGISTER_WITH_RANGE(function, queue, DuplicateInput)

REGISTER_INPUT_BENCHMARKS(BM_Build, CustomHeap2);
REGISTER_INPUT_BENCHMARKS(BM_Build, CustomHeap4);
REGISTER_INPUT_BENCHMARKS(BM_Build, CustomHeap8);
REGISTER_INPUT_BENCHMARKS(BM_Build, StandardHeap);

REGISTER_INPUT_BENCHMARKS(BM_Drain, CustomHeap2);
REGISTER_INPUT_BENCHMARKS(BM_Drain, CustomHeap4);
REGISTER_INPUT_BENCHMARKS(BM_Drain, CustomHeap8);
REGISTER_INPUT_BENCHMARKS(BM_Drain, StandardHeap);

#define REGISTER_MIXED_BENCHMARK(queue)          \
    BENCHMARK_TEMPLATE(BM_Mixed, queue)           \
        ->RangeMultiplier(4)                      \
        ->Range(1 << 10, 1 << 20)                 \
        ->Complexity()

REGISTER_MIXED_BENCHMARK(CustomHeap2);
REGISTER_MIXED_BENCHMARK(CustomHeap4);
REGISTER_MIXED_BENCHMARK(CustomHeap8);
REGISTER_MIXED_BENCHMARK(StandardHeap);

}  // namespace
