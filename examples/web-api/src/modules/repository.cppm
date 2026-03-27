// ============================================================================
// repository.cppm — Thread-safe in-memory store (Ch 1, 12, 15)
//
// Ch 1:  "RAII composes with exceptions, early returns, and scope."
// Ch 12: "Shared state must protect invariants, not just fields."
// Ch 15: "Contiguous storage is the default; prove alternatives."
//
// C++23 features used:
//   • C++20 modules (Ch 11)               — named module with imports
//   • std::shared_mutex / std::shared_lock — reader-writer locking
//   • std::scoped_lock                    — RAII lock acquisition
//   • std::expected / std::optional       — typed failure & absence
//   • std::ranges / std::views            — declarative queries
//   • std::vector                         — contiguous default storage
//   • [[nodiscard]]                       — prevent silent drops
//   • Concepts (requires)                 — constrain update callable
// ============================================================================
export module webapi.repository;

import std;

import webapi.error;
import webapi.task;

export namespace webapi {

// ── Concept for update functions (Ch 6: constrain callables) ────────────────
template <typename F>
concept TaskUpdater = std::invocable<F, Task&> &&
    requires(F f, Task& t) {
        { f(t) } -> std::same_as<void>;
    };

class TaskRepository {
public:
    // -- Create (Ch 3: return Result<T> so callers must handle failure) -------
    [[nodiscard]] Result<Task> create(Task task) {
        std::scoped_lock lock{mutex_};

        // Validate invariants before storing
        auto validated = Task::validate(std::move(task));
        if (!validated) return validated;

        validated->id = next_id_.fetch_add(1, std::memory_order_relaxed);
        tasks_.push_back(*validated);
        return *validated;
    }

    // -- Read one (Ch 5: use std::optional for "ordinary absence") ───────────
    [[nodiscard]] std::optional<Task> find_by_id(TaskId id) const {
        std::shared_lock lock{mutex_};
        auto it = std::ranges::find(tasks_, id, &Task::id);
        if (it == tasks_.end()) return std::nullopt;
        return *it;
    }

    // -- Read all (Ch 7 & 15: ranges over contiguous storage) ────────────────
    [[nodiscard]] std::vector<Task> find_all() const {
        std::shared_lock lock{mutex_};
        return tasks_;
    }

    // -- Filtered read (Ch 7: composable range pipelines) ────────────────────
    [[nodiscard]] std::vector<Task> find_completed(bool completed) const {
        std::shared_lock lock{mutex_};
        auto view = tasks_
            | std::views::filter([completed](const Task& t) {
                  return t.completed == completed;
              });
        return {view.begin(), view.end()};
    }

    // -- Update with callable (Ch 6: constrain the updater) ──────────────────
    template <TaskUpdater F>
    [[nodiscard]] Result<Task> update(TaskId id, F&& updater) {
        std::scoped_lock lock{mutex_};
        auto it = std::ranges::find(tasks_, id, &Task::id);
        if (it == tasks_.end()) {
            return make_error(ErrorCode::not_found,
                              std::format("task {} not found", id));
        }
        std::invoke(std::forward<F>(updater), *it);

        // Re-validate after mutation
        auto validated = Task::validate(*it);
        if (!validated) {
            // Rollback: caller's update broke an invariant — but we already
            // mutated in place. In production, copy-then-swap is safer.
            // This is simplified for the example.
            return validated;
        }
        return *it;
    }

    // -- Delete (returns whether something was actually removed) ─────────────
    [[nodiscard]] bool remove(TaskId id) {
        std::scoped_lock lock{mutex_};
        auto it = std::ranges::find(tasks_, id, &Task::id);
        if (it == tasks_.end()) return false;
        tasks_.erase(it);
        return true;
    }

    // -- Count (useful for health checks) ────────────────────────────────────
    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock{mutex_};
        return tasks_.size();
    }

private:
    mutable std::shared_mutex mutex_;
    std::vector<Task>         tasks_;      // Ch 15: contiguous default
    std::atomic<TaskId>       next_id_{1}; // monotonic ID generation
};

}  // namespace webapi
