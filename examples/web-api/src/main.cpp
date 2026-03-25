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
//   • std::jthread / std::stop_token — cooperative cancellation
//   • std::stop_source               — external stop signaling
//   • RAII throughout                 — automatic cleanup
//   • std::format                     — structured output
//   • Designated initializers         — readable construction
// ============================================================================

#include <atomic>
#include <csignal>
#include <cstdint>
#include <format>
#include <iostream>
#include <stop_token>
#include <thread>
#include <vector>

#include "webapi/handlers.hpp"
#include "webapi/http.hpp"
#include "webapi/middleware.hpp"
#include "webapi/repository.hpp"
#include "webapi/router.hpp"
#include "webapi/task.hpp"

// ── Signal handling for graceful shutdown ────────────────────────────────────
// Ch 14: "Cancellation must be explicit (tokens, stop sources)."
namespace {
    std::stop_source global_stop_source;  // NOLINT — intentional global

    extern "C" void signal_handler(int /*sig*/) {
        global_stop_source.request_stop();
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

    std::cout << std::format("Task API server starting on port {}\n", port);
    std::cout << "Press Ctrl+C to stop\n\n";
    std::cout << "Available endpoints:\n";
    std::cout << "  GET    /health        — health check\n";
    std::cout << "  GET    /tasks         — list all tasks\n";
    std::cout << "  GET    /tasks/:id     — get task by id\n";
    std::cout << "  POST   /tasks         — create a task\n";
    std::cout << "  PUT    /tasks/:id     — replace a task\n";
    std::cout << "  PATCH  /tasks/:id     — partial update\n";
    std::cout << "  DELETE /tasks/:id     — delete a task\n\n";

    // Run server on a jthread with cooperative cancellation (Ch 14).
    // The jthread owns a stop_source; we forward the global signal to it.
    // Ch 14: "Child tasks belong to parent scopes with bounded lifetimes."
    std::jthread server_thread{[&server](std::stop_token st) {
        server.run(st);
    }};

    // Forward the process-level stop signal to the jthread's stop_source.
    // Ch 14: std::stop_callback ties a callback to a stop_token's lifetime.
    std::stop_callback on_signal{global_stop_source.get_token(),
                                 [&server_thread]() noexcept {
                                     server_thread.request_stop();
                                 }};

    // Block main thread until the server thread completes.
    // The signal handler → global_stop_source → on_signal callback →
    // server_thread.request_stop() → server loop breaks → thread exits.
    server_thread.join();

    return 0;
}
