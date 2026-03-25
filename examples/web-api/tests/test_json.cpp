// ============================================================================
// test_json.cpp — Unit tests for JSON serialization utilities
// ============================================================================

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "webapi/json.hpp"
#include "webapi/task.hpp"

void test_extract_string_field() {
    auto result = webapi::json::extract_string_field(
        R"({"name":"Alice","age":"30"})", "name");
    assert(result.has_value());
    assert(result.value() == "Alice");
}

void test_extract_string_field_missing() {
    auto result = webapi::json::extract_string_field(
        R"({"name":"Alice"})", "email");
    assert(!result.has_value());
}

void test_extract_bool_field_true() {
    auto result = webapi::json::extract_bool_field(
        R"({"active":true})", "active");
    assert(result.has_value());
    assert(result.value() == true);
}

void test_extract_bool_field_false() {
    auto result = webapi::json::extract_bool_field(
        R"({"active":false})", "active");
    assert(result.has_value());
    assert(result.value() == false);
}

void test_extract_bool_field_missing() {
    auto result = webapi::json::extract_bool_field(
        R"({"name":"test"})", "active");
    assert(!result.has_value());
}

void test_serialize_array_empty() {
    std::vector<webapi::Task> empty;
    auto json = webapi::json::serialize_array(empty);
    assert(json == "[]");
}

void test_serialize_array_single() {
    std::vector<webapi::Task> tasks{
        webapi::Task{.id = 1, .title = "One", .description = "D"},
    };
    auto json = webapi::json::serialize_array(tasks);
    assert(json.front() == '[');
    assert(json.back() == ']');
    assert(json.find("\"id\":1") != std::string::npos);
}

void test_serialize_array_multiple() {
    std::vector<webapi::Task> tasks{
        webapi::Task{.id = 1, .title = "One"},
        webapi::Task{.id = 2, .title = "Two"},
    };
    auto json = webapi::json::serialize_array(tasks);
    // Should contain comma between items
    assert(json.find("},{") != std::string::npos);
}

void test_concept_satisfaction() {
    // Compile-time check: Task satisfies both concepts
    static_assert(webapi::json::JsonSerializable<webapi::Task>);
    static_assert(webapi::json::JsonDeserializable<webapi::Task>);
}

int main() {
    test_extract_string_field();
    test_extract_string_field_missing();
    test_extract_bool_field_true();
    test_extract_bool_field_false();
    test_extract_bool_field_missing();
    test_serialize_array_empty();
    test_serialize_array_single();
    test_serialize_array_multiple();
    test_concept_satisfaction();

    std::cout << "All JSON tests passed\n";
    return 0;
}
