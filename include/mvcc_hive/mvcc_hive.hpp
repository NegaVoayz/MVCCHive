#pragma once

#include <mvcc_hive/detail/commit_builder.hpp>
#include <mvcc_hive/detail/commit_store.hpp>
#include <mvcc_hive/detail/current_targets.hpp>
#include <mvcc_hive/detail/row_index.hpp>
#include <mvcc_hive/detail/snapshot_state.hpp>
#include <mvcc_hive/detail/transaction_overlay.hpp>
#include <mvcc_hive/detail/version_store.hpp>
#include <mvcc_hive/diff.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace mvcc_hive {

template<class T>
class MVCCHive {
public:
    using CommitId = mvcc_hive::CommitId;
    using VirtualId = mvcc_hive::VirtualId;
    using Diff = mvcc_hive::Diff;
    using DiffKind = mvcc_hive::DiffKind;

private:
    using TargetId = detail::TargetId;

public:
    class Snapshot {
    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) noexcept = default;
        Snapshot& operator=(Snapshot&&) noexcept = default;

        [[nodiscard]] CommitId commit_id() const noexcept {
            return commit_id_;
        }

        [[nodiscard]] bool valid() const noexcept {
            return guard_ && guard_->valid();
        }

        [[nodiscard]] bool contains(VirtualId id) const {
            return valid() && visible_target(id) != detail::no_target;
        }

        [[nodiscard]] const T& at(VirtualId id) const {
            if (!valid()) {
                throw std::logic_error("snapshot is no longer valid");
            }
            const auto& state = guard_->state();
            const detail::TargetId target = visible_target(id);
            if (target == detail::no_target) {
                throw std::out_of_range("virtual ID is not visible");
            }
            auto value = state.store.resolve(target);
            if (!value) {
                throw std::out_of_range("visible target is unavailable");
            }
            return value->get();
        }

        [[nodiscard]] std::optional<
            std::reference_wrapper<const T>>
        find(VirtualId id) const noexcept {
            if (!valid()) {
                return std::nullopt;
            }
            const detail::TargetId target = visible_target(id);
            if (target == detail::no_target) {
                return std::nullopt;
            }
            return guard_->state().store.resolve(target);
        }

        template<class F>
        void for_each(F&& fn) const {
            if (!valid()) {
                return;
            }

            const auto& state = guard_->state();
            const std::size_t count = state.rows.row_count();
            for (VirtualId id = 0; id < count; ++id) {
                const detail::TargetId target = visible_target(id);
                if (target == detail::no_target) {
                    continue;
                }
                auto value = state.store.resolve(target);
                if (value) {
                    std::invoke(fn, id, value->get());
                }
            }
        }

    private:
        friend class MVCCHive;
        friend class Transaction;

        Snapshot(
            CommitId commit_id,
            std::shared_ptr<detail::SnapshotGuard<T>> guard)
            : commit_id_(commit_id),
              guard_(std::move(guard)) {}

        [[nodiscard]] detail::TargetId visible_target(
            VirtualId id) const noexcept {
            return guard_->state().rows.visible_target(id, commit_id_);
        }

        CommitId commit_id_;
        std::shared_ptr<detail::SnapshotGuard<T>> guard_;
    };

    class Transaction {
    public:
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&&) = delete;
        Transaction& operator=(Transaction&&) = delete;

        ~Transaction() {
            if (active_) {
                rollback();
            }
        }

        template<class... Args>
        VirtualId emplace(Args&&... args) {
            ensure_active();
            if (owner_.next_virtual_id_ ==
                std::numeric_limits<VirtualId>::max()) {
                throw std::length_error("virtual ID space exhausted");
            }

            overlay_.prepare_insert(1);
            created_targets_.reserve(created_targets_.size() + 1);
            owner_.state_->store.reserve_reclamation(
                created_targets_.size() + 1);

            const TargetId target = owner_.state_->store.emplace(
                1, std::forward<Args>(args)...);
            const VirtualId id = owner_.next_virtual_id_;
            overlay_.set(id, target);
            created_targets_.push_back(target);
            owner_.next_virtual_id_ = id + 1;
            return id;
        }

        template<class... Args>
        void replace(VirtualId id, Args&&... args) {
            ensure_active();
            static_cast<void>(require_live(id));

            overlay_.prepare_insert(1);
            created_targets_.reserve(created_targets_.size() + 1);
            owner_.state_->store.reserve_reclamation(
                created_targets_.size() + 1);

            const TargetId target = owner_.state_->store.emplace(
                1, std::forward<Args>(args)...);
            overlay_.set(id, target);
            created_targets_.push_back(target);
        }

        void erase(VirtualId id) {
            ensure_active();
            static_cast<void>(require_live(id));
            overlay_.prepare_insert(1);
            overlay_.set(id, detail::no_target);
        }

        void swap(VirtualId lhs, VirtualId rhs) {
            ensure_active();
            const TargetId lhs_target = require_live(lhs);
            const TargetId rhs_target = require_live(rhs);
            if (lhs == rhs) {
                return;
            }

            overlay_.prepare_insert(2);
            overlay_.set(lhs, rhs_target);
            overlay_.set(rhs, lhs_target);
        }

        [[nodiscard]] const T& at(VirtualId id) const {
            if (!active_) {
                throw std::logic_error(
                    "transaction is no longer active");
            }

            const TargetId target = visible_target(id);
            if (target == detail::no_target) {
                throw std::out_of_range("virtual ID is not visible");
            }
            auto value = owner_.state_->store.resolve(target);
            if (!value) {
                throw std::out_of_range("visible target is unavailable");
            }
            return value->get();
        }

        [[nodiscard]] std::optional<
            std::reference_wrapper<const T>>
        find(VirtualId id) const noexcept {
            if (!active_) {
                return std::nullopt;
            }

            const TargetId target = visible_target(id);
            if (target == detail::no_target) {
                return std::nullopt;
            }
            return owner_.state_->store.resolve(target);
        }

        Snapshot commit() {
            ensure_active();

            std::vector<detail::DiffRecord> diffs;
            std::vector<TargetId> final_targets;
            diffs.reserve(overlay_.touched().size());
            final_targets.reserve(overlay_.touched().size());
            const CommitId parent_commit = parent_.commit_id();
            const CommitId next_commit = parent_commit + 1;

            for (const VirtualId id : overlay_.touched()) {
                TargetId current = detail::no_target;
                static_cast<void>(overlay_.find(id, current));
                const TargetId previous = parent_.visible_target(id);
                if (current == previous) {
                    continue;
                }

                diffs.push_back(detail::DiffRecord{
                    .commit_id = next_commit,
                    .virtual_id = id,
                    .previous_target = previous,
                    .current_target = current,
                });
                final_targets.push_back(current);
            }

            if (diffs.empty()) {
                discard_staged();
                active_ = false;
                writer_lock_.unlock();
                return parent_;
            }

            const std::size_t diff_count = diffs.size();
            owner_.state_->store.reserve_reclamation(
                created_targets_.size() + 1);

            std::unique_lock history_lock{owner_.history_mutex_};
            if (owner_.commits_.tip_commit() != parent_commit &&
                owner_.snapshots_->has_above(parent_commit)) {
                throw std::logic_error(
                    "cannot discard future commits while a snapshot pins them");
            }

            detail::CommitBuilder builder{
                next_commit,
                parent_commit,
                owner_.state_->rows,
                owner_.commits_,
                owner_.current_targets_,
                std::move(diffs),
            };
            builder.prepare();

            auto truncation = owner_.prepare_truncation(
                parent_commit, 1, diff_count);
            Snapshot result = owner_.make_snapshot(next_commit);

            owner_.apply_truncation(std::move(truncation));
            builder.publish();
            owner_.checkout_commit_ = next_commit;
            history_lock.unlock();

            std::vector<TargetId> transferred;
            std::vector<TargetId> acquired;
            transferred.reserve(final_targets.size());
            acquired.reserve(final_targets.size());
            for (const TargetId target : final_targets) {
                if (target == detail::no_target) {
                    continue;
                }
                if (is_created_target(target)) {
                    transferred.push_back(target);
                } else {
                    acquired.push_back(target);
                }
            }

            for (const TargetId target : acquired) {
                owner_.state_->store.acquire_view(target);
            }
            for (const TargetId target : created_targets_) {
                if (!contains_target(transferred, target)) {
                    owner_.state_->store.release_view(target);
                }
            }
            created_targets_.clear();
            owner_.state_->store.drain_retirements();
            active_ = false;
            writer_lock_.unlock();
            return result;
        }

        void rollback() noexcept {
            if (!active_) {
                return;
            }
            discard_staged();
            active_ = false;
            writer_lock_.unlock();
        }

    private:
        friend class MVCCHive;

        Transaction(
            MVCCHive& owner,
            std::unique_lock<std::mutex> writer_lock,
            Snapshot parent)
            : owner_(owner),
              writer_lock_(std::move(writer_lock)),
              parent_(std::move(parent)) {}

        void ensure_active() const {
            if (!active_) {
                throw std::logic_error(
                    "transaction is no longer active");
            }
        }

        [[nodiscard]] TargetId visible_target(
            VirtualId id) const noexcept {
            TargetId staged = detail::no_target;
            if (overlay_.find(id, staged)) {
                return staged;
            }
            return parent_.visible_target(id);
        }

        [[nodiscard]] TargetId require_live(VirtualId id) const {
            const TargetId target = visible_target(id);
            if (target == detail::no_target) {
                throw std::out_of_range("virtual ID is not visible");
            }
            return target;
        }

        [[nodiscard]] bool is_created_target(
            TargetId target) const noexcept {
            return contains_target(created_targets_, target);
        }

        [[nodiscard]] static bool contains_target(
            const std::vector<TargetId>& targets,
            TargetId target) noexcept {
            return std::find(
                       targets.begin(), targets.end(), target) !=
                   targets.end();
        }

        void discard_staged() noexcept {
            for (const TargetId target : created_targets_) {
                owner_.state_->store.release_view(target);
            }
            created_targets_.clear();
            owner_.state_->store.drain_retirements();
        }

        MVCCHive& owner_;
        std::unique_lock<std::mutex> writer_lock_;
        Snapshot parent_;
        detail::RowLockSet row_locks_;
        detail::TransactionOverlay overlay_;
        std::vector<TargetId> created_targets_;
        bool active_ = true;
    };

    MVCCHive()
        : state_(std::make_shared<detail::HiveState<T>>()),
          snapshots_(std::make_shared<detail::SnapshotRegistry>()) {}

    ~MVCCHive() {
        std::lock_guard lock{writer_mutex_};
        snapshots_.reset();
        state_.reset();
    }

    MVCCHive(const MVCCHive&) = delete;
    MVCCHive& operator=(const MVCCHive&) = delete;
    MVCCHive(MVCCHive&&) = delete;
    MVCCHive& operator=(MVCCHive&&) = delete;

    [[nodiscard]] Snapshot head() const {
        std::shared_lock lock{history_mutex_};
        return make_snapshot(checkout_commit_);
    }

    [[nodiscard]] std::optional<Snapshot> snapshot(
        CommitId id) const {
        std::shared_lock lock{history_mutex_};
        if (id > commits_.tip_commit()) {
            return std::nullopt;
        }
        if (!commits_.contains_commit(id)) {
            return std::nullopt;
        }
        return make_snapshot(id);
    }

    [[nodiscard]] Transaction begin_transaction() {
        std::unique_lock writer_lock{writer_mutex_};
        reap_deferred_if_idle();
        Snapshot parent = head();
        return Transaction{*this, std::move(writer_lock),
                           std::move(parent)};
    }

    [[nodiscard]] Transaction begin_transaction(CommitId id) {
        std::unique_lock writer_lock{writer_mutex_};
        reap_deferred_if_idle();
        if (id != checkout_commit_) {
            throw std::logic_error(
                "writable transactions must start at the current checkout");
        }
        auto parent = snapshot(id);
        if (!parent) {
            throw std::out_of_range("commit is not available");
        }
        return Transaction{*this, std::move(writer_lock),
                           std::move(*parent)};
    }

    // Moves the checkout to the parent of the current commit while retaining
    // the newer commits for redo.
    bool undo() {
        std::unique_lock writer_lock{writer_mutex_};
        std::unique_lock history_lock{history_mutex_};
        reap_deferred_if_idle();

        if (checkout_commit_ == detail::no_commit) {
            return false;
        }
        const auto index = commits_.commit_index(checkout_commit_);
        if (!index) {
            return false;
        }
        const CommitId parent = commits_.commit(*index).parent;
        if (parent != detail::no_commit &&
            !commits_.contains_commit(parent)) {
            return false;
        }

        auto cache = current_targets_.prepare_rebuild(
            state_->rows, parent);
        current_targets_.apply_rebuild(std::move(cache));
        checkout_commit_ = parent;
        return true;
    }

    // Moves the checkout to the next retained commit. A new commit after undo
    // discards the redo tail, so this returns false unless the tail is intact.
    bool redo() {
        std::unique_lock writer_lock{writer_mutex_};
        std::unique_lock history_lock{history_mutex_};
        reap_deferred_if_idle();

        CommitId next = detail::no_commit;
        if (checkout_commit_ == detail::no_commit) {
            next = commits_.first_commit_id();
            if (next == detail::no_commit) {
                return false;
            }
        } else {
            if (checkout_commit_ ==
                std::numeric_limits<CommitId>::max()) {
                return false;
            }
            next = checkout_commit_ + 1;
        }

        const auto index = commits_.commit_index(next);
        if (!index || commits_.commit(*index).parent != checkout_commit_) {
            return false;
        }

        auto cache = current_targets_.prepare_rebuild(
            state_->rows, next);
        current_targets_.apply_rebuild(std::move(cache));
        checkout_commit_ = next;
        return true;
    }

    [[nodiscard]] std::optional<Diff> diff(
        std::size_t index) const {
        std::shared_lock lock{history_mutex_};
        if (index >= commits_.diff_count()) {
            return std::nullopt;
        }
        return public_diff(commits_.diff(index));
    }

    [[nodiscard]] std::vector<Diff> diffs(CommitId id) const {
        std::shared_lock lock{history_mutex_};
        const auto index = commits_.commit_index(id);
        if (!index) {
            return {};
        }

        const detail::CommitRecord& record =
            commits_.commit(*index);

        std::vector<Diff> result;
        result.reserve(record.diff_count);
        for (std::size_t offset = 0;
             offset < record.diff_count;
             ++offset) {
            result.push_back(
                public_diff(
                    commits_.diff(record.first_diff + offset)));
        }
        return result;
    }

    void prune_before(CommitId first_retained) {
        std::unique_lock writer_lock{writer_mutex_};
        std::unique_lock history_lock{history_mutex_};
        reap_deferred_if_idle();

        if (commits_.tip_commit() == detail::no_commit) {
            return;
        }
        if (first_retained > checkout_commit_) {
            throw std::out_of_range(
                "first retained commit is beyond the checkout");
        }

        const CommitId first =
            std::max(first_retained, commits_.first_commit_id());
        auto commit_plan = commits_.prepare_prune(first);

        std::optional<CommitId> oldest_snapshot;
        CommitId snapshot_id = detail::no_commit;
        if (snapshots_->oldest(snapshot_id)) {
            oldest_snapshot = snapshot_id;
        }

        current_targets_.ensure_size(state_->rows.row_count());
        auto row_plan =
            state_->rows.prepare_prune(first, oldest_snapshot);
        owner_store_reserve(row_plan.released_targets().size());

        commits_.apply_prune(std::move(commit_plan));
        state_->rows.apply_prune(row_plan);
        release_targets(row_plan.released_targets());
        current_targets_.rebuild(state_->rows, checkout_commit_);
    }

    void clear_history() {
        std::unique_lock writer_lock{writer_mutex_};
        std::unique_lock history_lock{history_mutex_};
        reap_deferred_if_idle();

        if (commits_.tip_commit() == detail::no_commit) {
            return;
        }

        const CommitId checkout = checkout_commit_;
        auto truncation = prepare_truncation(checkout, 0, 0);
        detail::PrunedCommitState commit_plan;
        if (checkout != detail::no_commit) {
            commit_plan = commits_.prepare_clear_history(checkout);
        }

        std::optional<CommitId> oldest_snapshot;
        CommitId snapshot_id = detail::no_commit;
        if (snapshots_->oldest(snapshot_id)) {
            oldest_snapshot = snapshot_id;
        }

        current_targets_.ensure_size(state_->rows.row_count());
        auto row_plan =
            state_->rows.prepare_prune(checkout, oldest_snapshot);
        auto cache = current_targets_.prepare_rebuild(
            state_->rows, checkout);
        owner_store_reserve(row_plan.released_targets().size());

        apply_truncation(std::move(truncation));
        commits_.apply_prune(std::move(commit_plan));
        state_->rows.apply_prune(row_plan);
        release_targets(row_plan.released_targets());
        current_targets_.apply_rebuild(std::move(cache));
    }

    void clear() {
        std::unique_lock writer_lock{writer_mutex_};
        std::unique_lock history_lock{history_mutex_};

        auto fresh_state =
            std::make_shared<detail::HiveState<T>>();
        auto fresh_snapshots =
            std::make_shared<detail::SnapshotRegistry>();

        deferred_truncations_.clear();
        commits_.clear();
        snapshots_ = std::move(fresh_snapshots);
        state_ = std::move(fresh_state);
        current_targets_.clear();
        next_virtual_id_ = 0;
        checkout_commit_ = detail::no_commit;
    }

private:
    struct DeferredTruncation {
        std::vector<detail::RowNodeId> nodes;
        std::vector<TargetId> targets;
    };

    struct PreparedTruncation {
        bool active = false;
        detail::TruncatedCommitState commits;
        detail::RowIndex::TruncatePlan rows;
        std::vector<TargetId> current_targets;
    };

    [[nodiscard]] PreparedTruncation prepare_truncation(
        CommitId checkout,
        std::size_t additional_commits,
        std::size_t additional_diffs) {
        PreparedTruncation prepared;
        if (commits_.tip_commit() == checkout) {
            return prepared;
        }
        if (snapshots_->has_above(checkout)) {
            throw std::logic_error(
                "cannot discard future commits while a snapshot pins them");
        }

        prepared.commits = commits_.prepare_truncate_after(
            checkout, additional_commits, additional_diffs);
        prepared.rows = state_->rows.prepare_truncate(checkout);
        prepared.current_targets = current_targets_.prepare_rebuild(
            state_->rows, checkout);
        deferred_truncations_.reserve(
            deferred_truncations_.size() + 1);
        prepared.active = true;
        return prepared;
    }

    void apply_truncation(PreparedTruncation&& prepared) noexcept {
        if (!prepared.active) {
            return;
        }

        commits_.apply_truncate(std::move(prepared.commits));
        state_->rows.apply_truncate(prepared.rows);
        current_targets_.apply_rebuild(
            std::move(prepared.current_targets));
        deferred_truncations_.push_back(DeferredTruncation{
            .nodes = std::move(prepared.rows.released_nodes()),
            .targets = std::move(prepared.rows.released_targets()),
        });
    }

    void reap_deferred_if_idle() {
        if (deferred_truncations_.empty() || !snapshots_->empty()) {
            return;
        }
        for (DeferredTruncation& deferred : deferred_truncations_) {
            for (const detail::RowNodeId node : deferred.nodes) {
                state_->rows.release_node(node);
            }
            release_targets(deferred.targets);
        }
        deferred_truncations_.clear();
    }

    [[nodiscard]] static Diff public_diff(
        const detail::DiffRecord& record) noexcept {
        assert(record.previous_target != detail::no_target ||
               record.current_target != detail::no_target);
        DiffKind kind = DiffKind::update;
        if (record.previous_target == detail::no_target) {
            kind = DiffKind::insert;
        } else if (record.current_target == detail::no_target) {
            kind = DiffKind::remove;
        }
        return Diff{
            .commit_id = record.commit_id,
            .virtual_id = record.virtual_id,
            .kind = kind,
        };
    }

    [[nodiscard]] Snapshot make_snapshot(CommitId id) const {
        auto guard = std::make_shared<detail::SnapshotGuard<T>>(
            snapshots_, id, state_);
        snapshots_->add(id);
        guard->mark_registered();
        return Snapshot{id, std::move(guard)};
    }

    void owner_store_reserve(std::size_t additional) {
        state_->store.reserve_reclamation(additional);
    }

    void release_targets(const std::vector<TargetId>& targets) noexcept {
        for (const TargetId target : targets) {
            state_->store.release_view(target);
        }
        state_->store.drain_retirements();
    }

    std::shared_ptr<detail::HiveState<T>> state_;
    std::shared_ptr<detail::SnapshotRegistry> snapshots_;
    detail::CommitStore commits_;
    detail::CurrentTargetCache current_targets_;
    VirtualId next_virtual_id_ = 0;
    CommitId checkout_commit_ = detail::no_commit;
    std::vector<DeferredTruncation> deferred_truncations_;

    mutable std::mutex writer_mutex_;
    mutable std::shared_mutex history_mutex_;
};

}  // namespace mvcc_hive
