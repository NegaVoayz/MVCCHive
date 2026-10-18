#include <mvcc_hive/mvcc_hive.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Hive = mvcc_hive::MVCCHive<int>;

double microseconds(Clock::duration duration, std::size_t count) {
    return std::chrono::duration<double, std::micro>(duration).count() /
           static_cast<double>(count);
}

void run(std::size_t history_size, std::size_t sample_count) {
    Hive hive;
    auto first = hive.begin_transaction();
    const auto id = first.emplace(0);
    static_cast<void>(first.commit());

    const auto build_start = Clock::now();
    for (std::size_t generation = 1;
         generation <= history_size;
         ++generation) {
        auto tx = hive.begin_transaction();
        tx.replace(id, static_cast<int>(generation));
        static_cast<void>(tx.commit());
    }
    const auto build_end = Clock::now();

    const auto sample_start = Clock::now();
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        auto tx = hive.begin_transaction();
        tx.replace(
            id,
            static_cast<int>(history_size + sample + 1));
        static_cast<void>(tx.commit());
    }
    const auto sample_end = Clock::now();

    const auto undo_start = Clock::now();
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        if (!hive.undo()) {
            std::cerr << "undo unexpectedly exhausted history\n";
            return;
        }
        auto tx = hive.begin_transaction();
        tx.replace(
            id,
            static_cast<int>(history_size + sample_count + sample + 1));
        static_cast<void>(tx.commit());
    }
    const auto undo_end = Clock::now();

    std::cout << "history=" << history_size
              << " build_ms="
              << std::chrono::duration<double, std::milli>(
                     build_end - build_start)
                     .count()
              << " commit_us="
              << microseconds(sample_end - sample_start, sample_count)
              << " undo_commit_us="
              << microseconds(undo_end - undo_start, sample_count)
              << '\n';
}

}  // namespace

int main() {
    for (const std::size_t history : {5000U, 20000U, 50000U}) {
        run(history, 1000);
    }
}
