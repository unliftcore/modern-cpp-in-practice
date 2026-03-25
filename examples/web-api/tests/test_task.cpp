// ============================================================================
// test_task.cpp — Unit tests for Task domain model
// Ch 19: "Test the boundary — validate invariants hold."
// ============================================================================

#include <cassert>
#include <iostream>
#include <string>

import webapi.error;
import webapi.task;

void test_task_validation_accepts_valid() {
    auto result = webapi::Task::validate(webapi::Task{
        .title = "Valid Task",
        .description = "A description",
    });
    assert(result.has_value());
    assert(result->title == "Valid Task");
}

void test_task_validation_rejects_empty_title() {
    auto result = webapi::Task::validate(webapi::Task{
        .title = "",
    });
    assert(!result.has_value());
    assert(result.error().code == webapi::ErrorCode::bad_request);
}

void test_task_validation_rejects_long_title() {
    auto result = webapi::Task::validate(webapi::Task{
        .title = std::string(257, 'x'),
    });
    assert(!result.has_value());
    assert(result.error().code == webapi::ErrorCode::bad_request);
}

void test_task_to_json() {
    webapi::Task t{
        .id          = 42,
        .title       = "Test",
        .description = "Desc",
        .completed   = true,
    };
    auto json = t.to_json();
    assert(json.find("\"id\":42") != std::string::npos);
    assert(json.find("\"title\":\"Test\"") != std::string::npos);
    assert(json.find("\"completed\":true") != std::string::npos);
}

void test_task_from_json() {
    auto task = webapi::Task::from_json(
        R"({"title":"Hello","description":"World","completed":true})");
    assert(task.has_value());
    assert(task->title == "Hello");
    assert(task->description == "World");
    assert(task->completed == true);
}

void test_task_from_json_minimal() {
    auto task = webapi::Task::from_json(R"({"title":"Only title"})");
    assert(task.has_value());
    assert(task->title == "Only title");
    assert(task->description.empty());
    assert(task->completed == false);
}

void test_task_from_json_missing_title() {
    auto task = webapi::Task::from_json(R"({"description":"no title"})");
    assert(!task.has_value());
}

void test_task_equality() {
    webapi::Task a{.id = 1, .title = "A", .description = "D", .completed = false};
    webapi::Task b{.id = 1, .title = "A", .description = "D", .completed = false};
    webapi::Task c{.id = 2, .title = "A", .description = "D", .completed = false};
    assert(a == b);
    assert(a != c);
}

int main() {
    test_task_validation_accepts_valid();
    test_task_validation_rejects_empty_title();
    test_task_validation_rejects_long_title();
    test_task_to_json();
    test_task_from_json();
    test_task_from_json_minimal();
    test_task_from_json_missing_title();
    test_task_equality();

    std::cout << "All task tests passed\n";
    return 0;
}
