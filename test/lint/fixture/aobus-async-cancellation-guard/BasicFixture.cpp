// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <coroutine>
#include <exception>

namespace std
{
  struct NestedExceptionTypes
  {
    struct exception
    {};

    struct exception_ptr
    {
      exception_ptr& operator=(std::exception_ptr);
    };
  };
}

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

  bool isOperationCancelled(std::exception const& e) noexcept;
  [[noreturn]] void rethrowException(std::exception_ptr const& exceptionPtr);
  void rethrowIfOperationCancelled(std::exception const& e);
  void rethrowIfOperationCancelled();
}

namespace
{
  void report(std::exception const&);
  void reportUnknown();
  std::exception const& otherException();

  std::exception_ptr currentExceptionLookalike();

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

  ao::async::Task hasExhaustiveCancellationClassifier()
  {
    auto cancelled = false;

    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // NEGATIVE
    catch (std::exception const& e)
    {
      if (ao::async::isOperationCancelled(e))
      {
        cancelled = true;
      }
      else
      {
        report(e);
      }
    }

    if (cancelled)
    {
      reportUnknown();
    }
  }

  ao::async::Task classifierMustUseCatchVariable()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (std::exception const& e)
    {
      if (ao::async::isOperationCancelled(otherException()))
      {
        reportUnknown();
      }
      else
      {
        report(e);
      }
    }
  }

  ao::async::Task classifierRequiresElseBranch()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (std::exception const& e)
    {
      if (ao::async::isOperationCancelled(e))
      {
        reportUnknown();
      }

      report(e);
    }
  }

  ao::async::Task classifierRequiresNonEmptyBranches()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (std::exception const& e)
    {
      if (ao::async::isOperationCancelled(e))
      {
      }
      else
      {
        report(e);
      }
    }
  }

  ao::async::Task catchAllCannotUseClassifier()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (...)
    {
      if (ao::async::isOperationCancelled(otherException()))
      {
        reportUnknown();
      }
      else
      {
        reportUnknown();
      }
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

  ao::async::Task capturesCatchAllForDeferredHandling()
  {
    std::exception_ptr deferredException;

    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // NEGATIVE
    catch (...)
    {
      deferredException = std::current_exception();
      reportUnknown();
    }

    ao::async::rethrowException(deferredException);
  }

  ao::async::Task capturesStdExceptionForDeferredHandling()
  {
    std::exception_ptr deferredException;

    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // NEGATIVE
    catch (std::exception const&)
    {
      deferredException = std::current_exception();
      reportUnknown();
    }

    ao::async::rethrowException(deferredException);
  }

  ao::async::Task currentExceptionLookalikeIsRejected()
  {
    std::exception_ptr deferredException;

    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (...)
    {
      deferredException = currentExceptionLookalike();
      reportUnknown();
    }
  }

  ao::async::Task deferredHandlingMustComeFirst()
  {
    std::exception_ptr deferredException;

    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (...)
    {
      reportUnknown();
      deferredException = std::current_exception();
    }
  }

  ao::async::Task deferredHandlingMustOutliveCatch()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (...)
    {
      auto deferredException = std::current_exception();
      reportUnknown();
      static_cast<void>(deferredException);
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

  ao::async::Task exceptionNestedInStdRecordIsNotBroad()
  {
    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // NEGATIVE
    catch (std::NestedExceptionTypes::exception const&)
    {
      reportUnknown();
    }
  }

  ao::async::Task exceptionPtrNestedInStdRecordDoesNotOwnTheException()
  {
    std::NestedExceptionTypes::exception_ptr deferredException;

    try
    {
      co_await ao::async::resumeOnWorker();
    }
    // POSITIVE
    catch (...)
    {
      deferredException = std::current_exception();
      reportUnknown();
    }
  }
}
