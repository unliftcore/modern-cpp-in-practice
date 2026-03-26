// ============================================================================
// error.cppm — Failure representation (Ch 3: Errors & Results)
//
// "Distinguish failure from absence. Use std::expected for recoverable
//  errors and std::optional for ordinary absence."
//
// C++23 features used:
//   • C++20 modules (Ch 11)       — named module interface unit
//   • std::expected<T, E>         — typed, recoverable failure
//   • std::unexpected              — construct error values
//   • std::format                  — structured error messages
//   • std::string_view             — non-owning string borrowing
//   • [[nodiscard]]                — prevent silent error dropping
//   • constexpr                    — compile-time error code lookup
// ============================================================================
module;

// Global module fragment: standard headers used by the exported interface
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

export module webapi.error;

export namespace webapi {

// ── Error codes (closed set via enum class) ─────────────────────────────────
enum class ErrorCode : std::uint8_t {
    not_found,
    bad_request,
    conflict,
    internal_error,
};

// ── Compile-time HTTP status mapping (Ch 8: Compile-Time Programming) ───────
[[nodiscard]] constexpr int to_http_status(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::not_found:      return 404;
        case ErrorCode::bad_request:    return 400;
        case ErrorCode::conflict:       return 409;
        case ErrorCode::internal_error: return 500;
    }
    return 500;  // unreachable, but satisfies compiler
}

[[nodiscard]] constexpr std::string_view to_reason(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::not_found:      return "Not Found";
        case ErrorCode::bad_request:    return "Bad Request";
        case ErrorCode::conflict:       return "Conflict";
        case ErrorCode::internal_error: return "Internal Server Error";
    }
    return "Unknown";
}

// ── Error type (Ch 3: carry context alongside the category) ─────────────────
struct Error {
    ErrorCode   code;
    std::string detail;

    [[nodiscard]] int http_status() const noexcept {
        return to_http_status(code);
    }

    [[nodiscard]] std::string to_json() const {
        return std::format(
            R"({{"error":"{}","detail":"{}"}})",
            to_reason(code), detail
        );
    }
};

// ── Result alias (Ch 3: "make failure visible in the return type") ──────────
// [[nodiscard]] should be on functions returning Result<T>, enforced at call
// sites. The alias itself carries the intent.
template <typename T>
using Result = std::expected<T, Error>;

// ── Convenience factory (Ch 4: keep construction sites readable) ────────────
[[nodiscard]] inline std::unexpected<Error>
make_error(ErrorCode code, std::string detail) {
    return std::unexpected<Error>{Error{code, std::move(detail)}};
}

}  // namespace webapi
