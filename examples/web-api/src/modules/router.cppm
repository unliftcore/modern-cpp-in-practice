// ============================================================================
// router.cppm — HTTP request routing (Ch 9: Interface Design)
//
// "Make interfaces narrow — accept only what you need."
// "A good interface says what it does, not how."
//
// C++23 features used:
//   • C++20 modules (Ch 11)         — named module with imports
//   • std::string_view              — non-owning pattern matching
//   • std::vector + ranges          — route table scan
//   • std::function                 — type-erased handlers
//   • std::format                   — error messages
//   • [[nodiscard]]                 — prevent silent drops
//   • Designated initializers       — readable route registration
// ============================================================================
module;

#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module webapi.router;

import webapi.http;

export namespace webapi {

// ── Route entry ─────────────────────────────────────────────────────────────
struct Route {
    http::Method method;
    std::string  pattern;    // e.g., "/tasks" or "/tasks/"  (prefix match)
    bool         exact;      // true = exact match, false = prefix match
    http::Handler handler;
};

// ── Router: matches requests to handlers ────────────────────────────────────
class Router {
public:
    // Fluent registration API (Ch 9: readable construction)
    Router& get(std::string pattern, http::Handler h) {
        routes_.push_back({http::Method::GET, std::move(pattern), true, std::move(h)});
        return *this;
    }

    Router& get_prefix(std::string pattern, http::Handler h) {
        routes_.push_back({http::Method::GET, std::move(pattern), false, std::move(h)});
        return *this;
    }

    Router& post(std::string pattern, http::Handler h) {
        routes_.push_back({http::Method::POST, std::move(pattern), true, std::move(h)});
        return *this;
    }

    Router& put_prefix(std::string pattern, http::Handler h) {
        routes_.push_back({http::Method::PUT, std::move(pattern), false, std::move(h)});
        return *this;
    }

    Router& patch_prefix(std::string pattern, http::Handler h) {
        routes_.push_back({http::Method::PATCH, std::move(pattern), false, std::move(h)});
        return *this;
    }

    Router& delete_prefix(std::string pattern, http::Handler h) {
        routes_.push_back({http::Method::DELETE_, std::move(pattern), false, std::move(h)});
        return *this;
    }

    // Convert router to a single handler (for use with Server)
    [[nodiscard]] http::Handler to_handler() const {
        // Capture routes by value so the handler is self-contained
        auto routes = routes_;
        return [routes = std::move(routes)](const http::Request& req) -> http::Response {
            for (const auto& route : routes) {
                if (route.method != req.method) continue;

                if (route.exact) {
                    if (req.path == route.pattern) {
                        return route.handler(req);
                    }
                } else {
                    if (req.path.starts_with(route.pattern)) {
                        return route.handler(req);
                    }
                }
            }

            return http::Response::error(
                404,
                std::format(R"({{"error":"no route for {} {}"}})",
                            http::method_to_string(req.method), req.path)
            );
        };
    }

private:
    std::vector<Route> routes_;
};

}  // namespace webapi
