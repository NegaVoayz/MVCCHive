#include <mvcc_hive/detail/blocked_vector.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace {

using mvcc_hive::detail::BlockedVector;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "blocked_vector_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

void check_block_growth_and_values() {
    using Log = BlockedVector<std::uint64_t, 8>;
    static_assert(Log::block_capacity == 8);

    Log log;
    REQUIRE(log.empty());
    REQUIRE(log.capacity() == 0);

    log.reserve(1);
    REQUIRE(log.capacity() == 8);
    REQUIRE(log.size() == 0);

    for (std::uint64_t value = 0; value < 20; ++value) {
        log.push_back(value);
    }
    REQUIRE(log.size() == 20);
    REQUIRE(log.capacity() == 24);
    for (std::uint64_t value = 0; value < 20; ++value) {
        REQUIRE(log[value] == value);
    }

    const std::size_t stable_capacity = log.capacity();
    log.reserve(1);
    REQUIRE(log.capacity() == stable_capacity);
    log.append_prepared(20);
    REQUIRE(log.capacity() == stable_capacity);
    REQUIRE(log[20] == 20);
}

void check_move_and_swap() {
    using Log = BlockedVector<int, 4>;

    Log first;
    Log second;
    for (int value = 0; value < 6; ++value) {
        first.push_back(value);
    }
    for (int value = 100; value < 106; ++value) {
        second.push_back(value);
    }

    Log moved{std::move(first)};
    REQUIRE(moved.size() == 6);
    REQUIRE(moved[5] == 5);
    REQUIRE(first.empty());
    REQUIRE(first.capacity() == 0);

    moved.swap(second);
    REQUIRE(moved.size() == 6);
    REQUIRE(moved[0] == 100);
    REQUIRE(second.size() == 6);
    REQUIRE(second[0] == 0);

    moved.clear();
    REQUIRE(moved.empty());
    REQUIRE(moved.capacity() == 0);
}

void check_truncate() {
    using Log = BlockedVector<int, 8>;

    Log log;
    for (int value = 0; value < 20; ++value) {
        log.push_back(value);
    }
    REQUIRE(log.size() == 20);
    REQUIRE(log.capacity() == 24);

    log.truncate(10);
    REQUIRE(log.size() == 10);
    REQUIRE(log.capacity() == 16);
    for (int value = 0; value < 10; ++value) {
        REQUIRE(log[value] == value);
    }

    log.append_prepared(99);
    REQUIRE(log.size() == 11);
    REQUIRE(log[10] == 99);

    log.truncate(8, 9);
    REQUIRE(log.size() == 8);
    REQUIRE(log.capacity() == 16);
    log.append_prepared(100);
    REQUIRE(log.size() == 9);
    REQUIRE(log[8] == 100);

    log.truncate(0);
    REQUIRE(log.empty());
    REQUIRE(log.capacity() == 0);
    log.push_back(7);
    REQUIRE(log.size() == 1);
    REQUIRE(log[0] == 7);
}

}  // namespace

int main() {
    check_block_growth_and_values();
    check_move_and_swap();
    check_truncate();
}
