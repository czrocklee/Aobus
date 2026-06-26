// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/UiWorkflow.h"

#include <ao/Exception.h>
#include <ao/async/Executor.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    // A callback executor that queues dispatched work and runs it on the draining (test) thread, so that
    // "ran on the callback executor" is observable as "ran on the test thread".
    class QueuedExecutor final : public async::IExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }

      void dispatch(std::move_only_function<void()> task) override
      {
        auto const lock = std::scoped_lock{_mutex};
        _tasks.push_back(std::move(task));
      }

      void defer(std::move_only_function<void()> task) override { dispatch(std::move(task)); }

      bool runOne()
      {
        auto task = std::move_only_function<void()>{};

        {
          auto const lock = std::scoped_lock{_mutex};

          if (_tasks.empty())
          {
            return false;
          }

          task = std::move(_tasks.front());
          _tasks.pop_front();
        }

        task();
        return true;
      }

    private:
      mutable std::mutex _mutex;
      std::deque<std::move_only_function<void()>> _tasks;
    };

    struct WorkflowOwner final
    {
      std::atomic<bool> bodyEntered{false};
      std::atomic<bool> bodyFinished{false};
      std::atomic<int> handlerCalls{0};
      std::atomic<bool> sawAoException{false};
      std::atomic<std::thread::id> handlerThread{};
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
      owner->bodyFinished = true;
    }

    // Pumps the callback executor on the test thread until the predicate holds or the deadline passes.
    template<typename Predicate>
    bool pumpUntil(QueuedExecutor& executor,
                   Predicate const& predicate,
                   std::chrono::milliseconds timeout = std::chrono::seconds{2})
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (std::chrono::steady_clock::now() < deadline)
      {
        while (executor.runOne())
        {
        }

        if (predicate())
        {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      while (executor.runOne())
      {
      }

      return predicate();
    }
  } // namespace

  TEST_CASE("UiWorkflow - internal failure invokes the handler on the callback executor", "[gtk][common][uiworkflow]")
  {
    auto executor = QueuedExecutor{};
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

  TEST_CASE("UiWorkflow - successful body does not invoke the exception handler", "[gtk][common][uiworkflow]")
  {
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto scope = async::LifetimeScope{};
    auto owner = WorkflowOwner{};

    spawnUiWorkflow(
      runtime,
      scope,
      owner,
      [&runtime](WorkflowOwner* self) { return succeedingWorkflowBody(&runtime, self); },
      [](WorkflowOwner* self, std::exception_ptr /*exceptionPtr*/) { ++self->handlerCalls; });

    REQUIRE(pumpUntil(executor, [&owner] { return owner.bodyFinished.load(); }));

    // Drain any residual callback-executor work so a late handler dispatch would still be observed.
    REQUIRE_FALSE(
      pumpUntil(executor, [&owner] { return owner.handlerCalls.load() > 0; }, std::chrono::milliseconds{200}));

    CHECK(owner.bodyEntered.load());
    CHECK(owner.handlerCalls.load() == 0);

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::gtk::test
