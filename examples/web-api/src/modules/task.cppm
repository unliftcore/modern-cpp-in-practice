// ============================================================================
// task.cppm — Domain model with value semantics (Ch 2: Values & Identity)
//
// "If two objects with the same field values are interchangeable, the type
//  is a value. Make it regular: copyable, equality-comparable, orderable
//  if that makes domain sense."
//
// C++23 features used:
//   • C++20 modules (Ch 11)         — named module with inter-module imports
//   • Designated initializers       — readable construction
//   • std::string, std::string_view — owned vs. borrowed text
//   • std::optional                 — ordinary absence
//   • std::expected                 — recoverable failure
//   • std::format                   — structured serialization
//   • constexpr validation          — compile-time rule enforcement
//   • [[nodiscard]]                 — prevent silent drops
//   • operator<=>                   — defaulted three-way comparison
//   • Concepts (JsonSerializable / JsonDeserializable satisfaction)
// ============================================================================
module;

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

export module webapi.task;

import webapi.error;
import webapi.json;

export namespace webapi {

// ── Unique identifier (Ch 2: "identity needs an address or a key") ──────────
using TaskId = std::uint64_t;

// ── Domain value type ───────────────────────────────────────────────────────
struct Task {
    TaskId      id{0};
    std::string title;
    std::string description;
    bool        completed{false};

    // -- Value semantics: defaulted comparison (Ch 2) -------------------------
    [[nodiscard]] auto operator<=>(const Task&) const = default;

    // -- Invariant check (Ch 1: "state an invariant, enforce it") -------------
    [[nodiscard]] static Result<Task> validate(Task t) {
        if (t.title.empty()) {
            return make_error(ErrorCode::bad_request, "title must not be empty");
        }
        if (t.title.size() > 256) {
            return make_error(ErrorCode::bad_request, "title exceeds 256 characters");
        }
        return t;
    }

    // -- JSON serialization (satisfies json::JsonSerializable) ----------------
    [[nodiscard]] std::string to_json() const {
        return std::format(
            R"({{"id":{},"title":"{}","description":"{}","completed":{}}})",
            id, title, description, completed ? "true" : "false"
        );
    }

    // -- JSON deserialization (satisfies json::JsonDeserializable) -------------
    [[nodiscard]] static std::optional<Task> from_json(std::string_view sv) {
        auto title_opt = json::extract_string_field(sv, "title");
        if (!title_opt) return std::nullopt;

        auto desc = json::extract_string_field(sv, "description")
                        .value_or("");

        auto done = json::extract_bool_field(sv, "completed")
                        .value_or(false);

        return Task{
            .id          = 0,  // assigned by repository
            .title       = std::move(*title_opt),
            .description = std::move(desc),
            .completed   = done,
        };
    }
};

// ── Compile-time concept check (Ch 6: verify constraints are satisfied) ─────
static_assert(json::JsonSerializable<Task>,
              "Task must satisfy JsonSerializable");
static_assert(json::JsonDeserializable<Task>,
              "Task must satisfy JsonDeserializable");

}  // namespace webapi
