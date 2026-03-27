// ============================================================================
// handlers.cppm — HTTP request handlers (Ch 4, 9, 22)
//
// Ch 4:  "Accept parameters by the cheapest form that preserves intent."
// Ch 9:  "Translate domain errors at the boundary."
// Ch 22: "Building Services — wire layers, manage shutdown."
//
// C++23 features used:
//   • C++20 modules (Ch 11)       — named module with multi-module imports
//   • std::expected + monadic ops  — error propagation (and_then/transform)
//   • std::optional                — absence handling
//   • std::string_view             — borrowing path params
//   • std::format                  — response construction
//   • std::ranges                  — collection processing
//   • [[nodiscard]]                — prevent silent drops
//   • Structured bindings          — destructure results
// ============================================================================
module;

export module webapi.handlers;

import std;

import webapi.error;
import webapi.http;
import webapi.json;
import webapi.repository;
import webapi.task;

export namespace webapi::handlers {

// ── Helper: parse numeric path parameter (Ch 3: error at boundary) ──────────
[[nodiscard]] inline Result<TaskId>
parse_task_id(std::string_view segment) {
    TaskId id{};
    auto [ptr, ec] = std::from_chars(segment.data(),
                                     segment.data() + segment.size(),
                                     id);
    if (ec != std::errc{} || ptr != segment.data() + segment.size()) {
        return make_error(ErrorCode::bad_request,
                          std::format("invalid task id: '{}'", segment));
    }
    return id;
}

// ── Helper: translate Result<T> to HTTP Response at the boundary ────────────
// Ch 9: "Failure translation happens at layer boundaries."
template <json::JsonSerializable T>
[[nodiscard]] http::Response
result_to_response(const Result<T>& result, int success_status = 200) {
    if (result) {
        return {.status = success_status, .body = result->to_json()};
    }
    return http::Response::error(result.error().http_status(),
                                 result.error().to_json());
}

// ── GET /tasks — list all tasks ─────────────────────────────────────────────
[[nodiscard]] inline http::Handler
list_tasks(TaskRepository& repo) {
    return [&repo](const http::Request& /*req*/) -> http::Response {
        auto tasks = repo.find_all();
        return http::Response::ok(json::serialize_array(tasks));
    };
}

// ── GET /tasks/:id — get a single task ──────────────────────────────────────
[[nodiscard]] inline http::Handler
get_task(TaskRepository& repo) {
    return [&repo](const http::Request& req) -> http::Response {
        auto segment = req.path_param_after("/tasks/");
        if (!segment) {
            return http::Response::error(400,
                R"({"error":"missing task id"})");
        }

        auto id_result = parse_task_id(*segment);
        if (!id_result) {
            return http::Response::error(
                id_result.error().http_status(),
                id_result.error().to_json());
        }

        auto task = repo.find_by_id(*id_result);
        if (!task) {
            return http::Response::error(404,
                std::format(R"({{"error":"task {} not found"}})", *id_result));
        }

        return http::Response::ok(task->to_json());
    };
}

// ── POST /tasks — create a new task ─────────────────────────────────────────
[[nodiscard]] inline http::Handler
create_task(TaskRepository& repo) {
    return [&repo](const http::Request& req) -> http::Response {
        auto parsed = Task::from_json(req.body);
        if (!parsed) {
            return http::Response::error(400,
                R"({"error":"invalid JSON body"})");
        }

        auto result = repo.create(std::move(*parsed));
        return result_to_response(result, 201);
    };
}

// ── PUT /tasks/:id — full update ────────────────────────────────────────────
[[nodiscard]] inline http::Handler
update_task(TaskRepository& repo) {
    return [&repo](const http::Request& req) -> http::Response {
        auto segment = req.path_param_after("/tasks/");
        if (!segment) {
            return http::Response::error(400,
                R"({"error":"missing task id"})");
        }

        auto id_result = parse_task_id(*segment);
        if (!id_result) {
            return http::Response::error(
                id_result.error().http_status(),
                id_result.error().to_json());
        }

        auto parsed = Task::from_json(req.body);
        if (!parsed) {
            return http::Response::error(400,
                R"({"error":"invalid JSON body"})");
        }

        // Capture fields to update (Ch 4: move into lambda)
        auto new_title = std::move(parsed->title);
        auto new_desc  = std::move(parsed->description);
        auto new_done  = parsed->completed;

        auto result = repo.update(*id_result, [&](Task& t) {
            t.title       = std::move(new_title);
            t.description = std::move(new_desc);
            t.completed   = new_done;
        });

        return result_to_response(result);
    };
}

// ── PATCH /tasks/:id — partial update (toggle completion) ───────────────────
[[nodiscard]] inline http::Handler
patch_task(TaskRepository& repo) {
    return [&repo](const http::Request& req) -> http::Response {
        auto segment = req.path_param_after("/tasks/");
        if (!segment) {
            return http::Response::error(400,
                R"({"error":"missing task id"})");
        }

        auto id_result = parse_task_id(*segment);
        if (!id_result) {
            return http::Response::error(
                id_result.error().http_status(),
                id_result.error().to_json());
        }

        // Apply only fields present in body
        auto new_title = json::extract_string_field(req.body, "title");
        auto new_desc  = json::extract_string_field(req.body, "description");
        auto new_done  = json::extract_bool_field(req.body, "completed");

        auto result = repo.update(*id_result, [&](Task& t) {
            if (new_title) t.title       = std::move(*new_title);
            if (new_desc)  t.description = std::move(*new_desc);
            if (new_done)  t.completed   = *new_done;
        });

        return result_to_response(result);
    };
}

// ── DELETE /tasks/:id — remove a task ───────────────────────────────────────
[[nodiscard]] inline http::Handler
delete_task(TaskRepository& repo) {
    return [&repo](const http::Request& req) -> http::Response {
        auto segment = req.path_param_after("/tasks/");
        if (!segment) {
            return http::Response::error(400,
                R"({"error":"missing task id"})");
        }

        auto id_result = parse_task_id(*segment);
        if (!id_result) {
            return http::Response::error(
                id_result.error().http_status(),
                id_result.error().to_json());
        }

        if (repo.remove(*id_result)) {
            return http::Response::no_content();
        }

        return http::Response::error(404,
            std::format(R"({{"error":"task {} not found"}})", *id_result));
    };
}

// ── GET /health — health check endpoint ─────────────────────────────────────
[[nodiscard]] inline http::Handler
health_check(TaskRepository& repo) {
    return [&repo](const http::Request& /*req*/) -> http::Response {
        return http::Response::ok(
            std::format(R"({{"status":"ok","task_count":{}}})", repo.size())
        );
    };
}

}  // namespace webapi::handlers
