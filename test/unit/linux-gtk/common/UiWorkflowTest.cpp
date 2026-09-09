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

      bool tryWaitBodyFinished() const { return bodyFinished.tryWaitUntil(true); }
    };

    async::Task<void> succeedingWorkflowBodyAsync(async::Runtime* runtime,
                                                  WorkflowOwner* owner,
                                                  std::stop_token const stopToken)
    {
      owner->bodyEntered = true;
      owner->bodyEntryThread = std::this_thread::get_id();
      co_await runtime->resumeOnWorkerAsync(stopToken);
      owner->markBodyFinished();
    }

    async::Task<void> markUnexpectedEntryAsync(WorkflowOwner* owner, std::stop_token /*stopToken*/)
    {
      owner->bodyEntered = true;
      co_return;
    }

    async::Task<std::int32_t> produceResultAsync(async::Runtime* runtime)
    {
      co_await runtime->resumeOnWorkerAsync();
      co_return 42;
    }

    async::Task<std::int32_t> produceDelayedResultAsync(async::Runtime* runtime,
                                                        AsyncTestState<bool> entered,
                                                        AsyncBarrier* release)
    {
      co_await runtime->resumeOnWorkerAsync();
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
                    { return succeedingWorkflowBodyAsync(&runtime, self, stopToken); });

    REQUIRE(executor.tryWaitUntilQueued());
    executor.runUntilIdle();
    REQUIRE(owner.tryWaitBodyFinished());

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
                    { return markUnexpectedEntryAsync(self, stopToken); });

    REQUIRE(executor.tryWaitUntilQueued());
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
                produceResultAsync(&runtime),
                [](WorkflowOwner* self, std::int32_t const result)
                {
                  self->result = result;
                  self->completionThread = std::this_thread::get_id();
                  self->markBodyFinished();
                });

    REQUIRE(executor.tryWaitUntilQueued());
    REQUIRE(executor.tryDrainUntil([&owner] { return owner.bodyFinished.load(); }));
    REQUIRE(executor.tryDrainUntil([&scope] { return scope.empty(); }));

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
                produceDelayedResultAsync(&runtime, taskEntered, &releaseTask),
                [&completionCalled](WorkflowOwner*, std::int32_t) { completionCalled = true; });

    REQUIRE(executor.tryWaitUntilQueued());
    executor.runUntilIdle();
    auto const entered = taskEntered.tryWaitUntil(true);

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
    REQUIRE(executor.tryDrainUntil([&scope] { return scope.empty(); }));

    runtime.requestStop();
    runtime.join();

    CHECK_FALSE(completionCalled.load());
    CHECK(scope.empty());
  }
} // namespace ao::gtk::test
