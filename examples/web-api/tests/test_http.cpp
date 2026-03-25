// ============================================================================
// test_http.cpp — Unit tests for HTTP parsing and response formatting
// ============================================================================

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

import webapi.http;

void test_parse_method() {
    assert(webapi::http::parse_method("GET") == webapi::http::Method::GET);
    assert(webapi::http::parse_method("POST") == webapi::http::Method::POST);
    assert(webapi::http::parse_method("DELETE") == webapi::http::Method::DELETE_);
    assert(webapi::http::parse_method("INVALID") == webapi::http::Method::UNKNOWN);
}

void test_parse_request_get() {
    auto req = webapi::http::parse_request(
        "GET /tasks HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n");
    assert(req.has_value());
    assert(req->method == webapi::http::Method::GET);
    assert(req->path == "/tasks");
    assert(req->header("Host").has_value());
    assert(req->header("Host").value() == "localhost");
}

void test_parse_request_post_with_body() {
    auto req = webapi::http::parse_request(
        "POST /tasks HTTP/1.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        R"({"title":"New Task"})");
    assert(req.has_value());
    assert(req->method == webapi::http::Method::POST);
    assert(req->path == "/tasks");
    assert(req->body == R"({"title":"New Task"})");
}

void test_parse_request_malformed() {
    auto req = webapi::http::parse_request("not a valid request");
    assert(!req.has_value());
}

void test_path_param_after() {
    webapi::http::Request req{
        .method = webapi::http::Method::GET,
        .path = "/tasks/42",
    };
    auto param = req.path_param_after("/tasks/");
    assert(param.has_value());
    assert(param.value() == "42");
}

void test_path_param_after_missing() {
    webapi::http::Request req{
        .method = webapi::http::Method::GET,
        .path = "/tasks",
    };
    auto param = req.path_param_after("/tasks/");
    assert(!param.has_value());
}

void test_response_ok() {
    auto resp = webapi::http::Response::ok(R"({"status":"ok"})");
    assert(resp.status == 200);
    auto serialized = resp.serialize();
    assert(serialized.find("200 OK") != std::string::npos);
    assert(serialized.find(R"({"status":"ok"})") != std::string::npos);
}

void test_response_created() {
    auto resp = webapi::http::Response::created(R"({"id":1})");
    assert(resp.status == 201);
}

void test_response_no_content() {
    auto resp = webapi::http::Response::no_content();
    assert(resp.status == 204);
    assert(resp.body.empty());
}

void test_response_error() {
    auto resp = webapi::http::Response::error(404, R"({"error":"not found"})");
    assert(resp.status == 404);
}

void test_header_missing() {
    webapi::http::Request req{};
    auto h = req.header("Missing");
    assert(!h.has_value());
}

void test_socket_raii() {
    // Test that socket wrapper handles -1 correctly
    webapi::http::Socket s;
    assert(!s.valid());
    assert(s.fd() == -1);
}

void test_socket_move() {
    webapi::http::Socket s1{-1};
    assert(!s1.valid());

    // Move should transfer ownership
    webapi::http::Socket s2 = std::move(s1);
    assert(!s2.valid());
    assert(!s1.valid());
}

int main() {
    test_parse_method();
    test_parse_request_get();
    test_parse_request_post_with_body();
    test_parse_request_malformed();
    test_path_param_after();
    test_path_param_after_missing();
    test_response_ok();
    test_response_created();
    test_response_no_content();
    test_response_error();
    test_header_missing();
    test_socket_raii();
    test_socket_move();

    std::cout << "All HTTP tests passed\n";
    return 0;
}
