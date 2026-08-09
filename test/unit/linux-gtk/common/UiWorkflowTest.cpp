// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/UiWorkflow.h"

#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

namespace ao::gtk::test
{
  namespace
  {
    using rt::test::ManualExecutor;

    struct WorkflowOwner final
    {
      std::atomic<bool> bodyEntered{false};
      std::atomic<bool> bodyFinished{false};
      std::atomic<std::thread::id> bodyEntryThread{};
      std::mutex mutex;
      std::condition_variable cv;

      void markBodyFinished()
      {
        bodyFinished = true;
        cv.notify_all();
      }

      bool waitBodyFinished(std::chrono::milliseconds timeout = std::chrono::seconds{2})
      {
        auto lock = std::unique_lock{mutex};
        return cv.wait_for(lock, timeout, [this] { return bodyFinished.load(); });
      }
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
} // namespace ao::gtk::test
