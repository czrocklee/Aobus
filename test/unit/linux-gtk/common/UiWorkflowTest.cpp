// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/UiWorkflow.h"

#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

namespace ao::gtk::test
{
  namespace
  {
    using rt::test::AsyncBarrier;
    using rt::test::AsyncTestState;
    using rt::test::ManualExecutor;

    struct WorkflowOwner final
    {
      std::atomic<bool> bodyEntered{false};
      AsyncTestState<bool> bodyFinished = AsyncTestState<bool>::create(false);
      std::atomic<std::thread::id> bodyEntryThread{};
      std::atomic<std::int32_t> result{0};
      std::atomic<std::thread::id> completionThread{};

      void markBodyFinished() const { bodyFinished.set(true); }

      bool waitBodyFinished() const { return bodyFinished.waitUntil(true); }
    };

    async::Task<void> succeedingWorkflowBody(async::Runtime* runtime,
                                             WorkflowOwner* owner,
                                             std::stop_token const stopToken)
    {
      owner->bodyEntered = true;
      owner->bodyEntryThread = std::this_thread::get_id();
      co_await runtime->resumeOnWorker(stopToken);
      owner->markBodyFinished();
    }

    async::Task<void> markUnexpectedEntry(WorkflowOwner* owner, std::stop_token /*stopToken*/)
    {
      owner->bodyEntered = true;
      co_return;
    }

    async::Task<std::int32_t> produceResult(async::Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      co_return 42;
    }

    async::Task<std::int32_t> produceDelayedResult(async::Runtime* runtime,
                                                   AsyncTestState<bool> entered,
                                                   AsyncBarrier* release)
    {
      co_await runtime->resumeOnWorker();
      entered.set(true);
      release->wait();
      co_return 42;
    }
  } // namespace

  TEST_CASE("UiWorkflow - body enters on the callback executor", "[gtk][unit][uiworkflow][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiWorkflow(runtime,
                    scope,
                    owner,
                    "test UI workflow",
                    [&runtime](WorkflowOwner* self, std::stop_token const stopToken)
                    { return succeedingWorkflowBody(&runtime, self, stopToken); });

    REQUIRE(executor.waitUntilQueued());
    executor.runUntilIdle();
    REQUIRE(owner.waitBodyFinished());

    runtime.requestStop();
    runtime.join();

    CHECK(owner.bodyEntered.load());
    CHECK(owner.bodyEntryThread.load() == std::this_thread::get_id());
    CHECK(scope.empty());
  }

  TEST_CASE("UiWorkflow - cancellation before callback admission suppresses the body",
            "[gtk][regression][uiworkflow][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiWorkflow(runtime,
                    scope,
                    owner,
                    "test UI workflow cancellation",
                    [](WorkflowOwner* self, std::stop_token const stopToken)
                    { return markUnexpectedEntry(self, stopToken); });

    REQUIRE(executor.waitUntilQueued());
    scope.cancelAll();
    executor.runUntilIdle();

    runtime.requestStop();
    runtime.join();

    CHECK_FALSE(owner.bodyEntered.load());
    CHECK(scope.empty());
  }

  TEST_CASE("UiWorkflow - result tasks complete on the callback executor", "[gtk][unit][uiworkflow][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiTask(runtime,
                scope,
                owner,
                "test UI result workflow",
                produceResult(&runtime),
                [](WorkflowOwner* self, std::int32_t const result)
                {
                  self->result = result;
                  self->completionThread = std::this_thread::get_id();
                  self->markBodyFinished();
                });

    REQUIRE(executor.waitUntilQueued());
    REQUIRE(executor.drainUntil([&owner] { return owner.bodyFinished.load(); }));
    REQUIRE(executor.drainUntil([&scope] { return scope.empty(); }));

    runtime.requestStop();
    runtime.join();

    CHECK(owner.result.load() == 42);
    CHECK(owner.completionThread.load() == std::this_thread::get_id());
    CHECK(scope.empty());
  }

  TEST_CASE("UiWorkflow - owner cancellation suppresses a late result callback",
            "[gtk][regression][uiworkflow][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor};
    auto scope = async::LifetimeScope{};
    auto ownerPtr = std::make_unique<WorkflowOwner>();
    auto taskEntered = AsyncTestState<bool>::create(false);
    auto releaseTask = AsyncBarrier{};
    auto completionCalled = std::atomic<bool>{false};

    spawnUiTask(runtime,
                scope,
                *ownerPtr,
                "test late UI result",
                produceDelayedResult(&runtime, taskEntered, &releaseTask),
                [&completionCalled](WorkflowOwner*, std::int32_t) { completionCalled = true; });

    REQUIRE(executor.waitUntilQueued());
    executor.runUntilIdle();
    auto const entered = taskEntered.waitUntil(true);

    if (!entered)
    {
      releaseTask.release();
      runtime.requestStop();
      runtime.join();
    }

    REQUIRE(entered);

    scope.cancelAll();
    ownerPtr.reset();
    releaseTask.release();
    REQUIRE(executor.drainUntil([&scope] { return scope.empty(); }));

    runtime.requestStop();
    runtime.join();

    CHECK_FALSE(completionCalled.load());
    CHECK(scope.empty());
  }
} // namespace ao::gtk::test
