#pragma once
// ============================================================================
// middleware.hpp — Request pipeline with type erasure (Ch 10)
//
// Ch 10: "Type erasure lets you compose behaviors without inheritance
//         hierarchies. Store what an object does, not what it is."
//
// The middleware pipeline wraps a handler, transforming requests/responses
// without coupling middleware implementations to each other.
//
// C++23 features used:
//   • std::function               — type-erased callable storage
//   • std::move                   — efficient handler composition
//   • Concepts                    — constrain middleware signatures
//   • std::format                 — logging output
//   • std::chrono                 — timing middleware
//   • [[nodiscard]]               — prevent silent drops
// ============================================================================

#include <chrono>
#include <concepts>
#include <format>
#include <functional>
#include <iostream>
#include <string>
#include <utility>

#include "http.hpp"

namespace webapi::middleware {

// ── Middleware type: wraps the "next" handler ────────────────────────────────
// A middleware is a function that takes a handler and returns a new handler.
// This is the functional composition pattern (decorator).
using Middleware = std::function<http::Response(const http::Request&, const http::Handler&)>;

// ── Apply a middleware to a handler, producing a new handler ─────────────────
[[nodiscard]] inline http::Handler
apply(Middleware mw, http::Handler next) {
    return [mw = std::move(mw), next = std::move(next)](const http::Request& req) {
        return mw(req, next);
    };
}

// ── Chain multiple middlewares around a base handler ─────────────────────────
// Applied in reverse order so the first middleware in the list runs first.
template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, Middleware>
[[nodiscard]] http::Handler
chain(R&& middlewares, http::Handler base) {
    http::Handler current = std::move(base);
    // Apply in reverse so first middleware in list wraps outermost
    for (auto it = std::ranges::rbegin(middlewares);
         it != std::ranges::rend(middlewares); ++it)
    {
        current = apply(*it, std::move(current));
    }
    return current;
}

// ── Built-in middleware: request logging (Ch 21: Observability) ──────────────
inline Middleware request_logger() {
    return [](const http::Request& req, const http::Handler& next) -> http::Response {
        auto start = std::chrono::steady_clock::now();
        auto resp = next(req);
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

        std::cout << std::format("[{}] {} {} → {} ({} μs)\n",
            "LOG",
            http::method_to_string(req.method),
            req.path,
            resp.status,
            ms
        );
        return resp;
    };
}

// ── Built-in middleware: CORS headers ───────────────────────────────────────
inline Middleware cors(std::string allowed_origin = "*") {
    return [origin = std::move(allowed_origin)](
               const http::Request& req,
               const http::Handler& next) -> http::Response
    {
        auto resp = next(req);
        // In a real implementation, we'd modify response headers.
        // Here we demonstrate the pattern; the minimal HTTP layer
        // doesn't support per-response headers yet.
        (void)origin;  // used in production to set Access-Control-Allow-Origin
        return resp;
    };
}

// ── Built-in middleware: content-type enforcement ────────────────────────────
inline Middleware require_json() {
    return [](const http::Request& req, const http::Handler& next) -> http::Response {
        if (req.method == http::Method::POST || req.method == http::Method::PUT ||
            req.method == http::Method::PATCH)
        {
            auto ct = req.header("Content-Type");
            if (!ct || ct->find("application/json") == std::string_view::npos) {
                return http::Response::error(
                    400, R"({"error":"Content-Type must be application/json"})");
            }
        }
        return next(req);
    };
}

}  // namespace webapi::middleware
