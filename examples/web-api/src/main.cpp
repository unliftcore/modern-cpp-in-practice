// ============================================================================
// main.cpp — Application entry point (Ch 22: Building Services)
//
// "Wire layers in main, manage shutdown explicitly."
//
// This file demonstrates the complete service assembly:
//   1. Create domain objects (repository)
//   2. Register routes (handler layer)
//   3. Apply middleware (cross-cutting concerns)
//   4. Start server with cooperative cancellation (jthread + stop_token)
//   5. Handle SIGINT/SIGTERM for graceful shutdown
//
// C++23 features used:
//   • C++20 modules (Ch 11)           — import declarations
//   • std::jthread / std::stop_token  — cooperative cancellation (in Server)
//   • RAII throughout                 — automatic cleanup
//   • std::print / std::println       — structured output
//   • Designated initializers         — readable construction
// ============================================================================

#include <csignal>

import std;

import webapi.handlers;
import webapi.http;
import webapi.middleware;
import webapi.repository;
import webapi.router;
import webapi.task;

// ── Signal handling for graceful shutdown ────────────────────────────────────
// Ch 14: "Cancellation must be explicit."
namespace {
    std::atomic<bool> shutdown_requested{false};  // NOLINT — intentional global

    extern "C" void signal_handler(int /*sig*/) {
        shutdown_requested.store(true, std::memory_order_release);
    }
}

int main() {
    // -- 1. Install signal handlers (Ch 22: "manage shutdown explicitly") -----
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // -- 2. Create domain objects (Ch 1: RAII scoped lifetimes) ───────────────
    webapi::TaskRepository repo;

    // Seed with sample data
    (void)repo.create(webapi::Task{
        .title       = "Learn std::expected",
        .description = "Read Chapter 3 of Modern C++ in Practice",
    });
    (void)repo.create(webapi::Task{
        .title       = "Use concepts in APIs",
        .description = "Read Chapter 6 on template constraints",
    });
    (void)repo.create(webapi::Task{
        .title       = "Master ranges",
        .description = "Read Chapter 7 on ranges and generators",
        .completed   = true,
    });

    // -- 3. Set up routes (Ch 9: narrow, explicit interfaces) ────────────────
    webapi::Router router;
    router
        .get("/health",       webapi::handlers::health_check(repo))
        .get("/tasks",        webapi::handlers::list_tasks(repo))
        .get_prefix("/tasks/", webapi::handlers::get_task(repo))
        .post("/tasks",       webapi::handlers::create_task(repo))
        .put_prefix("/tasks/", webapi::handlers::update_task(repo))
        .patch_prefix("/tasks/", webapi::handlers::patch_task(repo))
        .delete_prefix("/tasks/", webapi::handlers::delete_task(repo));

    // -- 4. Apply middleware pipeline (Ch 10: type-erased composition) ────────
    std::vector<webapi::middleware::Middleware> pipeline{
        webapi::middleware::request_logger(),
        webapi::middleware::require_json(),
    };

    auto handler = webapi::middleware::chain(pipeline, router.to_handler());

    // -- 5. Start server (Ch 14: jthread with stop_token) ────────────────────
    constexpr std::uint16_t port = 8080;
    webapi::http::Server server{port, std::move(handler)};

    std::println("Task API server starting on port {}", port);
    std::println("Press Ctrl+C to stop");
    std::println("");
    std::println("Available endpoints:");
    std::println("  GET    /health        — health check");
    std::println("  GET    /tasks         — list all tasks");
    std::println("  GET    /tasks/:id     — get task by id");
    std::println("  POST   /tasks         — create a task");
    std::println("  PUT    /tasks/:id     — replace a task");
    std::println("  PATCH  /tasks/:id     — partial update");
    std::println("  DELETE /tasks/:id     — delete a task");
    std::println("");

    // Run server with cooperative cancellation (Ch 14: jthread + stop_token).
    // The jthread and stop_token mechanism lives inside Server::run_until(),
    // which internally creates a jthread and forwards the stop signal.
    server.run_until(shutdown_requested);

    return 0;
}
