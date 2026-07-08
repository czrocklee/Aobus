// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <print>
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
        std::println(stderr, "Unhandled exception in {} coroutine: {}", context, ex.what());
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

  Task<void> Runtime::resumeOnCallbackExecutor()
  {
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

    // Single cancellation checkpoint, after the executor hop. cancellation_state
    // is monotonic (a latched terminal signal never clears), so this post-resume
    // check subsumes a pre-resume one and additionally catches cancellation that
    // was signalled while the coroutine was in flight to the callback executor.
    auto const state = co_await boost::asio::this_coro::cancellation_state;

    if (state.cancelled() != boost::asio::cancellation_type::none)
    {
      throwOperationCancelled();
    }
  }

  Task<void> Runtime::resumeOnWorker()
  {
    try
    {
      co_await boost::asio::post(workerPool(), boost::asio::use_awaitable);
    }
    catch (boost::system::system_error const& e)
    {
      translateBoostCancellation(e);
    }

    // See resumeOnCallbackExecutor: one checkpoint after the hop is sufficient.
    auto const state = co_await boost::asio::this_coro::cancellation_state;

    if (state.cancelled() != boost::asio::cancellation_type::none)
    {
      throwOperationCancelled();
    }
  }

  void Runtime::spawnLogged(Task<void> task)
  {
    boost::asio::co_spawn(
      workerPool(), std::move(task), [](std::exception_ptr exPtr) { reportUnhandledException(exPtr, "root"); });
  }

  void Runtime::spawn(Task<void> task, CancellationSlot slot, std::function<void(std::exception_ptr)> callback)
  {
    boost::asio::co_spawn(
      workerPool(), std::move(task), boost::asio::bind_cancellation_slot(slot, std::move(callback)));
  }
} // namespace ao::async
