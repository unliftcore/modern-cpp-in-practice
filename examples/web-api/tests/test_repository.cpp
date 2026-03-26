// ============================================================================
// test_repository.cpp — Unit tests for TaskRepository
// Ch 19: "Test concurrent access and failure paths."
// ============================================================================

#include <cassert>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

import webapi.error;
import webapi.repository;
import webapi.task;

void test_create_and_find() {
    webapi::TaskRepository repo;

    auto result = repo.create(webapi::Task{.title = "First Task"});
    assert(result.has_value());
    assert(result->id > 0);
    assert(result->title == "First Task");

    auto found = repo.find_by_id(result->id);
    assert(found.has_value());
    assert(found->title == "First Task");
}

void test_find_missing() {
    webapi::TaskRepository repo;
    auto found = repo.find_by_id(999);
    assert(!found.has_value());
}

void test_find_all() {
    webapi::TaskRepository repo;
    (void)repo.create(webapi::Task{.title = "Task 1"});
    (void)repo.create(webapi::Task{.title = "Task 2"});

    auto all = repo.find_all();
    assert(all.size() == 2);
}

void test_find_completed() {
    webapi::TaskRepository repo;
    auto t1 = repo.create(webapi::Task{.title = "Done", .completed = true});
    (void)repo.create(webapi::Task{.title = "Not done"});

    auto done = repo.find_completed(true);
    assert(done.size() == 1);
    assert(done[0].title == "Done");

    auto pending = repo.find_completed(false);
    assert(pending.size() == 1);
}

void test_update() {
    webapi::TaskRepository repo;
    auto created = repo.create(webapi::Task{.title = "Original"});
    assert(created.has_value());

    auto updated = repo.update(created->id, [](webapi::Task& t) {
        t.title = "Updated";
        t.completed = true;
    });
    assert(updated.has_value());
    assert(updated->title == "Updated");
    assert(updated->completed == true);
}

void test_update_missing() {
    webapi::TaskRepository repo;
    auto result = repo.update(999, [](webapi::Task&) {});
    assert(!result.has_value());
    assert(result.error().code == webapi::ErrorCode::not_found);
}

void test_update_validates() {
    webapi::TaskRepository repo;
    auto created = repo.create(webapi::Task{.title = "Valid"});

    auto result = repo.update(created->id, [](webapi::Task& t) {
        t.title = "";  // breaks invariant
    });
    assert(!result.has_value());
    assert(result.error().code == webapi::ErrorCode::bad_request);
}

void test_remove() {
    webapi::TaskRepository repo;
    auto created = repo.create(webapi::Task{.title = "To Delete"});
    assert(created.has_value());

    assert(repo.remove(created->id) == true);
    assert(repo.find_by_id(created->id) == std::nullopt);
    assert(repo.size() == 0);
}

void test_remove_missing() {
    webapi::TaskRepository repo;
    assert(repo.remove(999) == false);
}

void test_concurrent_access() {
    webapi::TaskRepository repo;
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 100;

    std::vector<std::jthread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&repo, i]() {
            for (int j = 0; j < ops_per_thread; ++j) {
                auto title = std::format("Task-{}-{}", i, j);
                auto result = repo.create(webapi::Task{.title = std::move(title)});
                assert(result.has_value());
            }
        });
    }

    // jthreads auto-join on destruction
    threads.clear();

    assert(repo.size() == num_threads * ops_per_thread);
}

int main() {
    test_create_and_find();
    test_find_missing();
    test_find_all();
    test_find_completed();
    test_update();
    test_update_missing();
    test_update_validates();
    test_remove();
    test_remove_missing();
    test_concurrent_access();

    std::cout << "All repository tests passed\n";
    return 0;
}
