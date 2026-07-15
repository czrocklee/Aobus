// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/AsyncExceptionHandler.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Sleeper.h>
#include <ao/async/Task.h>

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp> // NOLINT(misc-include-cleaner) -- public Boost.Asio API header
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <print>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

namespace ao::async
{
  namespace
  {
    void writeUnhandledExceptionToStderr(std::exception_ptr const& exceptionPtr,
                                         std::string_view const context) noexcept
    {
      try
      {
        try
        {
          std::rethrow_exception(exceptionPtr);
        }
        catch (std::exception const& exception)
        {
          std::println(stderr, "Unhandled exception in {}: {}", context, exception.what());
        }
        catch (...)
        {
          std::println(stderr, "Unhandled unknown exception in {}", context);
        }
      }
      // NOLINTNEXTLINE(bugprone-empty-catch): The noexcept stderr fallback has no remaining diagnostic sink.
      catch (...)
      {
        // The final fallback must not escape through an Asio completion callback.
      }
    }

    [[noreturn]] void translateBoostCancellation(boost::system::system_error const& error)
    {
      if (error.code() == boost::asio::error::operation_aborted)
      {
        throwOperationCancelled();
      }

      throw;
    }

    Task<void> runCancellable(CancellableTask task, std::stop_token const stopToken)
    {
      throwIfStopRequested(stopToken);
      co_await task(stopToken);
    }

    Task<void> waitForTimer(std::chrono::milliseconds const delay, std::stop_token const stopToken)
    {
      throwIfStopRequested(stopToken);

      auto executor = co_await boost::asio::this_coro::executor;
      auto timerPtr = std::make_shared<boost::asio::steady_timer>(executor, delay);
      auto cancelTimer = std::stop_callback{
        stopToken,
        [executor, timerPtr] { boost::asio::dispatch(executor, [timerPtr] { std::ignore = timerPtr->cancel(); }); }};

      // A stop request can arrive between the first checkpoint and callback
      // registration. In that case cancel() precedes async_wait(), so do not
      // start a wait that would otherwise run until its original expiry.
      throwIfStopRequested(stopToken);

      try
      {
        co_await timerPtr->async_wait(boost::asio::use_awaitable);
      }
      catch (boost::system::system_error const& error)
      {
        if (stopToken.stop_requested() && error.code() == boost::asio::error::operation_aborted)
        {
          throwOperationCancelled();
        }

        translateBoostCancellation(error);
      }

      throwIfStopRequested(stopToken);
    }
  } // namespace

  void Runtime::handleUnhandledException(std::exception_ptr exceptionPtr, std::string_view const context) const noexcept
  {
    if (!exceptionPtr || isOperationCancelled(exceptionPtr))
    {
      return;
    }

    if (_exceptionHandler)
    {
      try
      {
        _exceptionHandler(exceptionPtr, context);
        return;
      }
      // NOLINTNEXTLINE(bugprone-empty-catch): A failed handler falls through to the stderr fallback below.
      catch (...)
      {
        // A diagnostic handler must not escape through an Asio completion callback.
      }
    }

    writeUnhandledExceptionToStderr(exceptionPtr, context);
  }

  Runtime::Runtime(Executor& callbackExecutor, AsyncExceptionHandler exceptionHandler, Sleeper* sleeper)
    : Runtime{callbackExecutor, std::max(1U, std::thread::hardware_concurrency()), std::move(exceptionHandler), sleeper}
  {
  }

  Runtime::Runtime(Executor& callbackExecutor,
                   std::size_t workerCount,
                   AsyncExceptionHandler exceptionHandler,
                   Sleeper* sleeper)
    : _callbackExecutor{callbackExecutor}
    , _exceptionHandler{std::move(exceptionHandler)}
    , _workerPool{workerCount}
    , _sleeper{sleeper}
  {
  }

  Runtime::~Runtime()
  {
    requestStop();
    join();
  }

  Executor& Runtime::callbackExecutor() noexcept
  {
    return _callbackExecutor;
  }

  void Runtime::reportUnhandledException(std::exception_ptr exceptionPtr, std::string_view const context) const noexcept
  {
    handleUnhandledException(std::move(exceptionPtr), context);
  }

  void Runtime::requestStop() noexcept
  {
    _workerPool.stop();
  }

  void Runtime::join()
  {
    _workerPool.join();
  }

  boost::asio::thread_pool& Runtime::workerPool() noexcept
  {
    return _workerPool;
  }

  Task<void> Runtime::resumeOnCallbackExecutor(std::stop_token const stopToken)
  {
    throwIfStopRequested(stopToken);

    try
    {
      co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void()>(
        [this](auto handler) { callbackExecutor().dispatch([cb = std::move(handler)] mutable { cb(); }); },
        boost::asio::use_awaitable);
    }
    catch (boost::system::system_error const& e)
    {
      translateBoostCancellation(e);
    }

    throwIfStopRequested(stopToken);
  }

  Task<void> Runtime::resumeOnWorker(std::stop_token const stopToken)
  {
    throwIfStopRequested(stopToken);

    try
    {
      co_await boost::asio::post(workerPool(), boost::asio::use_awaitable);
    }
    catch (boost::system::system_error const& e)
    {
      translateBoostCancellation(e);
    }

    throwIfStopRequested(stopToken);
  }

  Task<void> Runtime::sleepFor(std::chrono::milliseconds const delay, std::stop_token const stopToken)
  {
    gsl_Expects(delay > std::chrono::milliseconds::zero());
    throwIfStopRequested(stopToken);

    if (_sleeper != nullptr)
    {
      co_await _sleeper->sleepFor(delay, stopToken);
    }
    else
    {
      auto executor = co_await boost::asio::this_coro::executor;
      auto timerExecutor = boost::asio::make_strand(executor);
      co_await boost::asio::co_spawn(timerExecutor, waitForTimer(delay, stopToken), boost::asio::use_awaitable);
    }

    throwIfStopRequested(stopToken);
  }

  void Runtime::spawnLogged(Task<void> task)
  {
    boost::asio::co_spawn( // NOLINT(misc-include-cleaner) -- declared by the public co_spawn header
      workerPool(),
      std::move(task),
      [this](std::exception_ptr exceptionPtr) { handleUnhandledException(std::move(exceptionPtr), "root coroutine"); });
  }

  std::move_only_function<void()> Runtime::startCancellable(CancellableTask task,
                                                            std::function<void(std::exception_ptr)> completion)
  {
    auto stopSourcePtr = std::make_shared<std::stop_source>();
    boost::asio::co_spawn( // NOLINT(misc-include-cleaner) -- declared by the public co_spawn header
      workerPool(),
      runCancellable(std::move(task), stopSourcePtr->get_token()),
      [completion = std::move(completion)](std::exception_ptr exPtr) { completion(exPtr); });
    return [stopSourcePtr] { std::ignore = stopSourcePtr->request_stop(); };
  }

  TaskHandle Runtime::spawnCancellable(CancellableTask task)
  {
    return TaskHandle{
      startCancellable(std::move(task),
                       [this](std::exception_ptr exceptionPtr)
                       { handleUnhandledException(std::move(exceptionPtr), "cancellable coroutine"); })};
  }
} // namespace ao::async
