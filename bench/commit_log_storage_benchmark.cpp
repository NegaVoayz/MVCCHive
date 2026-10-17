#include <mvcc_hive/detail/blocked_vector.hpp>
#include <mvcc_hive/detail/commit_store.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <iostream>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using mvcc_hive::Diff;
using mvcc_hive::detail::BlockedVector;
using mvcc_hive::detail::CommitRecord;

constexpr std::size_t sample_count = 1000;

template<class Container>
void append_one(Container& values, typename Container::value_type value) {
    values.push_back(value);
}

template<class Container>
void append_amortized(Container& values) {
    const auto needed = values.size() + 1;
    if (needed > values.capacity()) {
        values.reserve(std::max(needed, values.capacity() * 2));
    }
    values.push_back(typename Container::value_type{});
}

double microseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count() /
           static_cast<double>(sample_count);
}

template<std::size_t BlockCapacity>
double blocked_vector_microseconds(std::size_t history_size) {
    BlockedVector<CommitRecord, BlockCapacity> commits;
    BlockedVector<Diff, BlockCapacity> diffs;
    for (std::size_t i = 0; i < history_size; ++i) {
        commits.push_back(CommitRecord{});
        diffs.push_back(Diff{});
    }
    const auto start = Clock::now();
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        commits.reserve(1);
        commits.append_prepared(CommitRecord{});
        diffs.reserve(1);
        diffs.append_prepared(Diff{});
    }
    return microseconds(Clock::now() - start);
}

void run(std::size_t history_size) {
    std::vector<CommitRecord> exact_commits(history_size);
    std::vector<Diff> exact_diffs(history_size);
    const auto exact_start = Clock::now();
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        const auto needed = exact_commits.size() + 1;
        exact_commits.reserve(needed);
        exact_diffs.reserve(exact_diffs.size() + 1);
        append_one(exact_commits, CommitRecord{});
        append_one(exact_diffs, Diff{});
    }
    const auto exact_end = Clock::now();

    std::vector<CommitRecord> grown_commits(history_size);
    std::vector<Diff> grown_diffs(history_size);
    const auto grown_start = Clock::now();
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        append_amortized(grown_commits);
        append_amortized(grown_diffs);
    }
    const auto grown_end = Clock::now();

    std::deque<CommitRecord> deque_commits(history_size);
    std::deque<Diff> deque_diffs(history_size);
    const auto deque_start = Clock::now();
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        append_one(deque_commits, CommitRecord{});
        append_one(deque_diffs, Diff{});
    }
    const auto deque_end = Clock::now();

    std::cout << "history=" << history_size
              << " exact_vector_us=" << microseconds(exact_end - exact_start)
              << " grown_vector_us=" << microseconds(grown_end - grown_start)
              << " deque_us=" << microseconds(deque_end - deque_start)
              << " blocked_512_us="
              << blocked_vector_microseconds<512>(history_size)
              << " blocked_2048_us="
              << blocked_vector_microseconds<2048>(history_size)
              << " blocked_8192_us="
              << blocked_vector_microseconds<8192>(history_size)
              << '\n';
}

}  // namespace

int main() {
    for (const std::size_t history : {5000U, 20000U, 50000U}) {
        run(history);
    }
}
