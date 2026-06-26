// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <coroutine>
#include <exception>

namespace ao::async
{
  class OperationCancelled final : public std::exception
  {};

  struct Task
  {
    struct promise_type
    {
      Task get_return_object();
      void return_void();
      void unhandled_exception();
      std::suspend_never initial_suspend();
      std::suspend_never final_suspend() noexcept;
    };
  };

  struct Awaitable
  {
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<>) const noexcept;
    void await_resume() const noexcept;
  };

  Awaitable resumeOnWorker();

  void rethrowIfOperationCancelled(std::exception const& e);
  void rethrowIfOperationCancelled();
}

namespace
{
  void report(std::exception const&);
  void reportUnknown();
  std::exception const& otherException();

  ao::async::Task missingStdExceptionGuard()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (std::exception const& e)
    {
      report(e);
    }
  }

  ao::async::Task hasStdExceptionGuard()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // NEGATIVE
    catch (std::exception const& e)
    {
      ao::async::rethrowIfOperationCancelled(e);
      report(e);
    }
  }

  ao::async::Task wrongStdExceptionGuardArgument()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (std::exception const& e)
    {
      ao::async::rethrowIfOperationCancelled(otherException());
      report(e);
    }
  }

  ao::async::Task explicitCancellationCatchDoesNotCoverBoostCancellation()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    catch (ao::async::OperationCancelled const&)
    {
      throw;
    }
    // POSITIVE
    catch (std::exception const& e)
    {
      report(e);
    }
  }

  ao::async::Task missingCatchAllGuard()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (...)
    {
      reportUnknown();
    }
  }

  ao::async::Task hasCatchAllGuard()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // NEGATIVE
    catch (...)
    {
      ao::async::rethrowIfOperationCancelled();
      reportUnknown();
    }
  }

  ao::async::Task wrongCatchAllGuardArgument()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (...)
    {
      ao::async::rethrowIfOperationCancelled(otherException());
      reportUnknown();
    }
  }

  void nonCoroutineBroadCatchIsOutOfScope()
  {
    try
    {
      reportUnknown();
    }
    // NEGATIVE
    catch (std::exception const& e)
    {
      report(e);
    }
  }

  ao::async::Task nestedNonCoroutineLambdaBroadCatchIsOutOfScope()
  {
    auto const reportLater = []
    {
      try
      {
        reportUnknown();
      }
      // NEGATIVE
      catch (std::exception const& e)
      {
        report(e);
      }
    };

    reportLater();
    co_await ao::async::resumeOnWorker();
  }

  ao::async::Task nestedCoroutineLambdaBroadCatchIsChecked()
  {
    auto const reportLater = []() -> ao::async::Task
    {
      try
      {
        co_await ao::async::resumeOnWorker();
      }
      // POSITIVE
      catch (std::exception const& e)
      {
        report(e);
      }
    };

    reportLater();
    co_await ao::async::resumeOnWorker();
  }
}
