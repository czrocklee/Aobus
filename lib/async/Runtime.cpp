// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
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
#include <thread>
#include <utility>

namespace ao::async
{
  namespace
  {
    void reportUnhandledException(std::exception_ptr exPtr, char const* context)
    {
      if (!exPtr)
      {
        return;
      }

      try
      {
        std::rethrow_exception(exPtr);
      }
      catch (std::exception const& ex)
      {
        if (!isOperationCancelled(ex))
        {
          std::println(stderr, "Unhandled exception in {} coroutine: {}", context, ex.what());
        }
      }
      catch (...)
      {
        std::println(stderr, "Unhandled unknown exception in {} coroutine", context);
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

  Runtime::Runtime(Executor& callbackExecutor)
    : Runtime{callbackExecutor, std::max(1U, std::thread::hardware_concurrency())}
  {
  }

  Runtime::Runtime(Executor& callbackExecutor, std::size_t workerCount)
    : _callbackExecutor{callbackExecutor}, _workerPool{workerCount}
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

    if (_sleepForOverride)
    {
      co_await _sleepForOverride(delay, stopToken);
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
      [](std::exception_ptr exPtr) { reportUnhandledException(exPtr, "root"); });
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
    return TaskHandle{startCancellable(
      std::move(task), [](std::exception_ptr exPtr) { reportUnhandledException(exPtr, "cancellable"); })};
  }
} // namespace ao::async
