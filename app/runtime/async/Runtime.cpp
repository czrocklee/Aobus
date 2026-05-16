// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Runtime.h"
#include <ao/utility/Log.h>
// NOLINTBEGIN(misc-include-cleaner)
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
// NOLINTEND(misc-include-cleaner)
#include <thread>

// NOLINTBEGIN(misc-include-cleaner)
namespace ao::rt::async
{
  Runtime::Runtime(rt::IControlExecutor& uiExecutor)
    : _uiExecutor{uiExecutor}, _workerPool{std::thread::hardware_concurrency()}
  {
  }

  Runtime::~Runtime()
  {
    requestStop();
    join();
  }

  rt::IControlExecutor& Runtime::uiExecutor() noexcept
  {
    return _uiExecutor;
  }

  void Runtime::requestStop() noexcept
  {
    _stopRequested = true;
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

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  Task<void> resumeOnUi(Runtime& runtime)
  {
    co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void()>(
      [&runtime](auto handler)
      {
        runtime.uiExecutor().dispatch(
          // NOLINTNEXTLINE(readability-identifier-length)
          [cb = std::move(handler)] mutable { cb(); });
      },
      boost::asio::use_awaitable);
  }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  Task<void> resumeOnWorker(Runtime& runtime)
  {
    auto state = co_await boost::asio::this_coro::cancellation_state;

    if (state.cancelled() != boost::asio::cancellation_type::none)
    {
      throw boost::system::system_error(boost::asio::error::operation_aborted);
    }

    co_await boost::asio::post(runtime.workerPool(), boost::asio::use_awaitable);

    state = co_await boost::asio::this_coro::cancellation_state;

    if (state.cancelled() != boost::asio::cancellation_type::none)
    {
      throw boost::system::system_error(boost::asio::error::operation_aborted);
    }
  }

  void spawnLogged(Runtime& runtime, Task<void> task)
  {
    boost::asio::co_spawn(runtime.workerPool(),
                          std::move(task),
                          // NOLINTNEXTLINE(readability-identifier-length)
                          [](std::exception_ptr exPtr)
                          {
                            if (exPtr)
                            {
                              try
                              {
                                std::rethrow_exception(exPtr);
                              }
                              catch (std::exception const& ex)
                              {
                                APP_LOG_ERROR("Unhandled exception in root coroutine: {}", ex.what());
                              }
                              catch (...)
                              {
                                APP_LOG_ERROR("Unhandled unknown exception in root coroutine");
                              }
                            }
                          });
  }

  template<typename T>
  std::future<T> spawn(Runtime& runtime, Task<T> task)
  {
    return boost::asio::co_spawn(runtime.workerPool(), std::move(task), boost::asio::use_future);
  }

  // Explicit instantiations for common types used in tests
  template std::future<void> spawn(Runtime&, Task<void>);
  template std::future<std::thread::id> spawn(Runtime&, Task<std::thread::id>);

  // NOLINTEND(misc-include-cleaner)
} // namespace ao::rt::async
