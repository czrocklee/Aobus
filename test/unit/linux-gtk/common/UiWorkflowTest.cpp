// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/UiWorkflow.h"

#include "test/unit/RuntimeTestSupport.h"
#include <ao/Exception.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>

namespace ao::gtk::test
{
  namespace
  {
    using rt::test::AsyncExceptionRecorder;
    using rt::test::ManualExecutor;
    using rt::test::requireSingleRecordedException;
    constexpr auto kTestExceptionContext = std::string_view{"test UI workflow"};

    struct WorkflowOwner final
    {
      std::atomic<bool> bodyEntered{false};
      std::atomic<bool> bodyFinished{false};
      std::atomic<int> handlerCalls{0};
      std::atomic<std::thread::id> handlerThread{};
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

    // Worker-side bodies live as free coroutines (taking the runtime by pointer) so the spawn-site lambdas stay
    // plain, non-coroutine adapters, matching the production workflow pattern and avoiding capturing-coroutine UB.
    async::Task<void> failingWorkflowBody(async::Runtime* runtime,
                                          WorkflowOwner* owner,
                                          std::stop_token const stopToken)
    {
      owner->bodyEntered = true;
      co_await runtime->resumeOnWorker(stopToken);
      // Throw from worker code: the boundary must marshal back before presenting it.
      throwException<Exception>("boom");
    }

    async::Task<void> succeedingWorkflowBody(async::Runtime* runtime,
                                             WorkflowOwner* owner,
                                             std::stop_token const stopToken)
    {
      owner->bodyEntered = true;
      co_await runtime->resumeOnWorker(stopToken);
      owner->markBodyFinished();
    }

    // Pumps the callback executor on the test thread until the predicate holds.
    template<typename Predicate>
    bool pumpUntil(ManualExecutor& executor, Predicate const& predicate)
    {
      while (!predicate())
      {
        REQUIRE(executor.waitUntilQueued());
        executor.runUntilIdle();
      }

      return true;
    }
  } // namespace

  TEST_CASE("UiWorkflow - internal failure invokes the handler on the callback executor",
            "[gtk][unit][uiworkflow][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto exceptionRecorder = AsyncExceptionRecorder{};
    auto runtime = async::Runtime{executor, exceptionRecorder.handler()};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiWorkflow(
      runtime,
      scope,
      owner,
      kTestExceptionContext,
      [&runtime](WorkflowOwner* self, std::stop_token const stopToken)
      { return failingWorkflowBody(&runtime, self, stopToken); },
      [](WorkflowOwner* self)
      {
        self->handlerThread = std::this_thread::get_id();
        ++self->handlerCalls;
      });

    REQUIRE(pumpUntil(executor, [&owner] { return owner.handlerCalls.load() > 0; }));

    CHECK(owner.bodyEntered.load());
    CHECK(owner.handlerCalls.load() == 1);
    CHECK(owner.handlerThread.load() == std::this_thread::get_id());

    runtime.requestStop();
    runtime.join();

    requireSingleRecordedException<Exception>(exceptionRecorder, kTestExceptionContext);
  }

  TEST_CASE("UiWorkflow - successful body does not invoke the exception handler",
            "[gtk][unit][uiworkflow][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto exceptionRecorder = AsyncExceptionRecorder{};
    auto runtime = async::Runtime{executor, exceptionRecorder.handler()};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiWorkflow(
      runtime,
      scope,
      owner,
      kTestExceptionContext,
      [&runtime](WorkflowOwner* self, std::stop_token const stopToken)
      { return succeedingWorkflowBody(&runtime, self, stopToken); },
      [](WorkflowOwner* self) { ++self->handlerCalls; });

    REQUIRE(executor.waitUntilQueued());
    executor.runUntilIdle();
    REQUIRE(owner.waitBodyFinished());

    // Drain residual callback-executor work so a same-turn handler dispatch would still be observed.
    executor.runUntilIdle();

    CHECK(owner.bodyEntered.load());
    CHECK(owner.handlerCalls.load() == 0);
    CHECK(exceptionRecorder.snapshot().empty());

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("UiWorkflow - cancellation cannot erase a fault captured before presentation",
            "[gtk][regression][uiworkflow][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto exceptionRecorder = AsyncExceptionRecorder{};
    auto runtime = async::Runtime{executor, exceptionRecorder.handler()};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiWorkflow(
      runtime,
      scope,
      owner,
      kTestExceptionContext,
      [&runtime](WorkflowOwner* self, std::stop_token const stopToken)
      { return failingWorkflowBody(&runtime, self, stopToken); },
      [](WorkflowOwner* self) { ++self->handlerCalls; });

    REQUIRE(executor.waitUntilQueued());
    REQUIRE(executor.runOne());
    REQUIRE(exceptionRecorder.waitForCount(1));
    REQUIRE(executor.waitUntilQueued());

    scope.cancelAll();
    executor.runUntilIdle();

    CHECK(owner.handlerCalls.load() == 0);

    runtime.requestStop();
    runtime.join();

    requireSingleRecordedException<Exception>(exceptionRecorder, kTestExceptionContext);
  }
} // namespace ao::gtk::test
