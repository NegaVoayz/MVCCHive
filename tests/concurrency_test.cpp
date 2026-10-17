#include <mvcc_hive/mvcc_hive.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

namespace {

struct Cell {
    Cell(int row, int version)
        : row(row),
          version(version),
          mirror(version) {}

    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;
    Cell(Cell&&) = delete;
    Cell& operator=(Cell&&) = delete;

    int row;
    int version;
    int mirror;
};

using MVCCHive = mvcc_hive::MVCCHive<Cell>;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "concurrency_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

void check_snapshot_values(
    const MVCCHive::Snapshot& snapshot,
    std::atomic<int>& errors) {
    if (!snapshot.valid()) {
        errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    snapshot.for_each([&](MVCCHive::VirtualId id, const Cell& value) {
        if (value.row != static_cast<int>(id) ||
            value.version != value.mirror) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }
    });
}

void check_one_writer_and_readers() {
    constexpr std::size_t row_count = 32;
    constexpr int commit_count = 400;

    MVCCHive hive;
    {
        auto tx = hive.begin_transaction();
        for (std::size_t i = 0; i < row_count; ++i) {
            static_cast<void>(
                tx.emplace(static_cast<int>(i), 0));
        }
        static_cast<void>(tx.commit());
    }

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> readers;
    readers.reserve(4);

    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stop.load(std::memory_order_relaxed)) {
                auto snapshot = hive.head();
                check_snapshot_values(snapshot, errors);
                std::this_thread::yield();
            }
        });
    }

    std::mt19937 rng{0x51a7c0deU};
    auto old_shared = hive.head();
    start.store(true, std::memory_order_release);

    for (int generation = 1;
         generation <= commit_count;
         ++generation) {
        auto tx = hive.begin_transaction();
        for (int change = 0; change < 4; ++change) {
            const auto row =
                static_cast<MVCCHive::VirtualId>(
                    rng() % row_count);
            tx.replace(
                row,
                static_cast<int>(row),
                generation);
        }
        auto committed = tx.commit();
        REQUIRE(committed.valid());
        check_snapshot_values(committed, errors);

        if (generation % 16 == 0) {
            const auto first =
                committed.commit_id() > 8
                    ? committed.commit_id() - 8
                    : committed.commit_id();
            hive.prune_before(first);
        }
        if (generation % 48 == 0) {
            hive.clear_history();
        }
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) {
        reader.join();
    }

    REQUIRE(errors.load(std::memory_order_relaxed) == 0);
    REQUIRE(old_shared.valid());
    check_snapshot_values(old_shared, errors);
    REQUIRE(errors.load(std::memory_order_relaxed) == 0);

    auto final = hive.head();
    REQUIRE(final.valid());
    check_snapshot_values(final, errors);
    REQUIRE(errors.load(std::memory_order_relaxed) == 0);
}

void check_final_view_retirement() {
    MVCCHive hive;
    {
        auto tx = hive.begin_transaction();
        static_cast<void>(tx.emplace(0, 0));
        static_cast<void>(tx.commit());
    }

    for (int generation = 1; generation < 100; ++generation) {
        auto tx = hive.begin_transaction();
        tx.replace(0, 0, generation);
        auto committed = tx.commit();
        if (generation > 20) {
            hive.prune_before(committed.commit_id() - 10);
        }
    }

    std::atomic<int> errors{0};
    auto final = hive.head();
    check_snapshot_values(final, errors);
    REQUIRE(errors.load(std::memory_order_relaxed) == 0);
}

}  // namespace

int main() {
    check_one_writer_and_readers();
    check_final_view_retirement();
}
