#include <mvcc_hive/mvcc_hive.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using MVCCHive = mvcc_hive::MVCCHive<int>;
using CommitId = MVCCHive::CommitId;
using VirtualId = MVCCHive::VirtualId;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "model_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

using State = std::map<VirtualId, int>;

struct HeldSnapshot {
    CommitId commit_id;
    MVCCHive::Snapshot snapshot;
    State state;
};

void check_snapshot(
    const MVCCHive::Snapshot& snapshot,
    const State& expected,
    VirtualId id_count,
    int line) {
    require(snapshot.valid(), "snapshot.valid()", line);
    for (VirtualId id = 0; id < id_count; ++id) {
        const auto expected_it = expected.find(id);
        const auto actual = snapshot.find(id);
        require(
            actual.has_value() == (expected_it != expected.end()),
            "snapshot.find presence",
            line);
        if (actual) {
            require(
                actual->get() == expected_it->second,
                "snapshot.find value",
                line);
        }
    }

    std::size_t visited = 0;
    snapshot.for_each([&](VirtualId id, const int& value) {
        const auto it = expected.find(id);
        require(it != expected.end(), "snapshot.for_each id", line);
        require(it->second == value, "snapshot.for_each value", line);
        ++visited;
    });
    require(visited == expected.size(), "snapshot.for_each count", line);
}

class ReferenceModel {
public:
    explicit ReferenceModel(std::uint64_t seed)
        : rng_(seed) {
        history_.emplace(0, State{});
    }

    void run(std::size_t steps) {
        for (std::size_t step = 0; step < steps; ++step) {
            const unsigned action = static_cast<unsigned>(rng_() % 10);
            if (action < 6) {
                apply_transaction();
            } else if (action == 6) {
                apply_prune();
            } else if (action == 7) {
                apply_clear_history();
            } else if (action == 8) {
                apply_clear();
            } else {
                check_everything();
            }
            check_everything();
        }
    }

private:
    [[nodiscard]] std::vector<VirtualId> live_ids() const {
        std::vector<VirtualId> ids;
        ids.reserve(current_.size());
        for (const auto& [id, value] : current_) {
            static_cast<void>(value);
            ids.push_back(id);
        }
        return ids;
    }

    [[nodiscard]] int next_value() {
        return static_cast<int>(rng_() & 0x7fffffffU);
    }

    void maybe_hold(
        const MVCCHive::Snapshot& snapshot,
        State state) {
        if (head_ != 0 && snapshot.valid() && rng_() % 4 == 0 &&
            held_.size() < 6) {
            held_.push_back(HeldSnapshot{
                .commit_id = snapshot.commit_id(),
                .snapshot = snapshot,
                .state = std::move(state),
            });
        }
        if (held_.size() == 6 && rng_() % 3 == 0) {
            held_.erase(
                held_.begin() + static_cast<std::ptrdiff_t>(rng_() % 6));
        }
    }

    void apply_transaction() {
        auto tx = hive_.begin_transaction();
        State next = current_;
        const std::size_t operations = 1 + rng_() % 4;

        for (std::size_t operation = 0;
             operation < operations;
             ++operation) {
            const unsigned kind = static_cast<unsigned>(rng_() % 4);
            if (kind == 0 && next.size() < 48) {
                const int value = next_value();
                const VirtualId id = tx.emplace(value);
                REQUIRE(id == next_id_);
                next[id] = value;
                ++next_id_;
                continue;
            }

            if (next.empty()) {
                continue;
            }

            std::vector<VirtualId> ids;
            ids.reserve(next.size());
            for (const auto& [id, value] : next) {
                static_cast<void>(value);
                ids.push_back(id);
            }

            if (kind == 1) {
                const VirtualId id = ids[rng_() % ids.size()];
                const int value = next_value();
                tx.replace(id, value);
                next[id] = value;
            } else if (kind == 2) {
                const VirtualId id = ids[rng_() % ids.size()];
                tx.erase(id);
                next.erase(id);
            } else if (ids.size() >= 2) {
                const std::size_t first = rng_() % ids.size();
                std::size_t second = rng_() % ids.size();
                if (first == second) {
                    second = (second + 1) % ids.size();
                }
                const VirtualId lhs = ids[first];
                const VirtualId rhs = ids[second];
                tx.swap(lhs, rhs);
                std::swap(next[lhs], next[rhs]);
            }
        }

        if (rng_() % 11 == 0) {
            tx.rollback();
            check_head();
            return;
        }

        const bool changed = next != current_;
        auto result = tx.commit();
        const CommitId expected_head =
            changed ? head_ + 1 : head_;
        REQUIRE(result.commit_id() == expected_head);
        if (changed) {
            head_ = expected_head;
            history_[head_] = next;
        }
        current_ = std::move(next);
        check_snapshot(result, current_, next_id_, __LINE__);
        maybe_hold(result, current_);
    }

    void apply_prune() {
        if (head_ == 0) {
            return;
        }

        std::vector<CommitId> available;
        available.reserve(history_.size());
        for (const auto& [id, state] : history_) {
            static_cast<void>(state);
            available.push_back(id);
        }
        const CommitId first =
            available[rng_() % available.size()];
        hive_.prune_before(first);

        for (auto it = history_.begin(); it != history_.end();) {
            if (it->first < first) {
                it = history_.erase(it);
            } else {
                ++it;
            }
        }
        check_head();
    }

    void apply_clear_history() {
        if (head_ == 0) {
            return;
        }
        hive_.clear_history();
        history_.clear();
        history_.emplace(head_, current_);
    }

    void apply_clear() {
        hive_.clear();
        for (const HeldSnapshot& held : held_) {
            REQUIRE(!held.snapshot.valid());
            REQUIRE(!held.snapshot.find(0).has_value());
        }
        held_.clear();
        current_.clear();
        history_.clear();
        history_.emplace(0, State{});
        next_id_ = 0;
        head_ = 0;
    }

    void check_head() {
        auto snapshot = hive_.head();
        REQUIRE(snapshot.commit_id() == head_);
        check_snapshot(snapshot, current_, next_id_, __LINE__);
    }

    void check_everything() {
        check_head();
        for (const HeldSnapshot& held : held_) {
            REQUIRE(held.snapshot.valid());
            check_snapshot(
                held.snapshot, held.state, next_id_, __LINE__);
        }
        for (const auto& [id, state] : history_) {
            if (id == 0) {
                continue;
            }
            auto snapshot = hive_.snapshot(id);
            REQUIRE(snapshot.has_value());
            check_snapshot(*snapshot, state, next_id_, __LINE__);
        }
    }

    std::mt19937_64 rng_;
    MVCCHive hive_;
    State current_;
    std::map<CommitId, State> history_;
    std::vector<HeldSnapshot> held_;
    VirtualId next_id_ = 0;
    CommitId head_ = 0;
};

}  // namespace

int main() {
    ReferenceModel{0x4d564343ULL}.run(600);
    ReferenceModel{0x8f3a91d7ULL}.run(600);
    ReferenceModel{0xc001d00dULL}.run(600);
}
