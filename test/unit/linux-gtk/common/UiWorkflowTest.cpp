// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/UiWorkflow.h"

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Exception.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
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
      std::atomic<int> handlerCalls{0};
      std::atomic<bool> sawAoException{false};
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
    async::Task<void> failingWorkflowBody(async::Runtime* runtime, WorkflowOwner* owner)
    {
      owner->bodyEntered = true;
      co_await runtime->resumeOnWorker();
      // Throw from worker code: the boundary must marshal back before presenting it.
      throwException<Exception>("boom");
    }

    async::Task<void> succeedingWorkflowBody(async::Runtime* runtime, WorkflowOwner* owner)
    {
      owner->bodyEntered = true;
      co_await runtime->resumeOnWorker();
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
            "[gtk][unit][common][uiworkflow]")
  {
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiWorkflow(
      runtime,
      scope,
      owner,
      [&runtime](WorkflowOwner* self) { return failingWorkflowBody(&runtime, self); },
      [](WorkflowOwner* self, std::exception_ptr exceptionPtr)
      {
        self->handlerThread = std::this_thread::get_id();

        // The handler must receive the original failure type. Record it explicitly: a wrong-type rethrow
        // escapes here and fails the test via pumpUntil rather than silently passing on handlerCalls alone.
        try
        {
          std::rethrow_exception(exceptionPtr);
        }
        catch (Exception const&)
        {
          self->sawAoException = true;
        }

        ++self->handlerCalls;
      });

    REQUIRE(pumpUntil(executor, [&owner] { return owner.handlerCalls.load() > 0; }));

    CHECK(owner.bodyEntered.load());
    CHECK(owner.handlerCalls.load() == 1);
    CHECK(owner.sawAoException.load());
    CHECK(owner.handlerThread.load() == std::this_thread::get_id());

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("UiWorkflow - successful body does not invoke the exception handler", "[gtk][unit][common][uiworkflow]")
  {
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiWorkflow(
      runtime,
      scope,
      owner,
      [&runtime](WorkflowOwner* self) { return succeedingWorkflowBody(&runtime, self); },
      [](WorkflowOwner* self, std::exception_ptr /*exceptionPtr*/) { ++self->handlerCalls; });

    REQUIRE(executor.waitUntilQueued());
    executor.runUntilIdle();
    REQUIRE(owner.waitBodyFinished());

    // Drain residual callback-executor work so a same-turn handler dispatch would still be observed.
    executor.runUntilIdle();

    CHECK(owner.bodyEntered.load());
    CHECK(owner.handlerCalls.load() == 0);

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::gtk::test
