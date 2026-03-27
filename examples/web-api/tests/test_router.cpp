// ============================================================================
// test_router.cpp — Unit tests for request routing
// ============================================================================

#include <cassert>

import std;

import webapi.http;
import webapi.router;

void test_exact_route_match() {
    webapi::Router router;
    router.get("/test", [](const webapi::http::Request&) {
        return webapi::http::Response::ok(R"({"matched":true})");
    });

    auto handler = router.to_handler();

    webapi::http::Request req{
        .method = webapi::http::Method::GET,
        .path   = "/test",
    };
    auto resp = handler(req);
    assert(resp.status == 200);
    assert(resp.body.find("matched") != std::string::npos);
}

void test_prefix_route_match() {
    webapi::Router router;
    router.get_prefix("/items/", [](const webapi::http::Request& req) {
        auto param = req.path_param_after("/items/");
        return webapi::http::Response::ok(
            std::format(R"({{"id":"{}"}})", param.value_or("none")));
    });

    auto handler = router.to_handler();

    webapi::http::Request req{
        .method = webapi::http::Method::GET,
        .path   = "/items/42",
    };
    auto resp = handler(req);
    assert(resp.status == 200);
    assert(resp.body.find("42") != std::string::npos);
}

void test_no_route_match() {
    webapi::Router router;
    router.get("/exists", [](const webapi::http::Request&) {
        return webapi::http::Response::ok("{}");
    });

    auto handler = router.to_handler();

    webapi::http::Request req{
        .method = webapi::http::Method::GET,
        .path   = "/does-not-exist",
    };
    auto resp = handler(req);
    assert(resp.status == 404);
}

void test_method_mismatch() {
    webapi::Router router;
    router.get("/only-get", [](const webapi::http::Request&) {
        return webapi::http::Response::ok("{}");
    });

    auto handler = router.to_handler();

    webapi::http::Request req{
        .method = webapi::http::Method::POST,
        .path   = "/only-get",
    };
    auto resp = handler(req);
    assert(resp.status == 404);
}

void test_multiple_routes() {
    webapi::Router router;
    router
        .get("/a", [](const webapi::http::Request&) {
            return webapi::http::Response::ok(R"({"route":"a"})");
        })
        .post("/b", [](const webapi::http::Request&) {
            return webapi::http::Response::created(R"({"route":"b"})");
        });

    auto handler = router.to_handler();

    webapi::http::Request req_a{.method = webapi::http::Method::GET, .path = "/a"};
    assert(handler(req_a).status == 200);

    webapi::http::Request req_b{.method = webapi::http::Method::POST, .path = "/b"};
    assert(handler(req_b).status == 201);
}

int main() {
    test_exact_route_match();
    test_prefix_route_match();
    test_no_route_match();
    test_method_mismatch();
    test_multiple_routes();

    std::println("All router tests passed");
    return 0;
}
