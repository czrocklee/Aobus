// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Runtime.h"

#include "ao/utility/Log.h"
#include "runtime/CorePrimitives.h"
#include "runtime/async/Task.h"

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#include <exception>
#include <functional>
#include <future>
#include <thread>
#include <utility>

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

  rt::IControlExecutor& Runtime::controlExecutor() noexcept
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

  Task<void> Runtime::resumeOnControl()
  {
    co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void()>(
      [this](auto handler) { controlExecutor().dispatch([cb = std::move(handler)] mutable { cb(); }); },
      boost::asio::use_awaitable);
  }

  Task<void> Runtime::resumeOnWorker()
  {
    auto state = co_await boost::asio::this_coro::cancellation_state;

    if (state.cancelled() != boost::asio::cancellation_type::none)
    {
      throw boost::system::system_error{boost::asio::error::operation_aborted};
    }

    co_await boost::asio::post(workerPool(), boost::asio::use_awaitable);

    state = co_await boost::asio::this_coro::cancellation_state;

    if (state.cancelled() != boost::asio::cancellation_type::none)
    {
      throw boost::system::system_error{boost::asio::error::operation_aborted};
    }
  }

  void Runtime::spawnLogged(Task<void> task)
  {
    boost::asio::co_spawn(workerPool(),
                          std::move(task),
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

  void Runtime::spawn(Task<void> task, CancellationSlot slot, std::function<void(std::exception_ptr)> callback)
  {
    boost::asio::co_spawn(
      workerPool(), std::move(task), boost::asio::bind_cancellation_slot(slot, std::move(callback)));
  }

  template<typename T>
  std::future<T> Runtime::spawn(Task<T> task)
  {
    return boost::asio::co_spawn(workerPool(), std::move(task), boost::asio::use_future);
  }

  // Explicit instantiations for common types used in tests
  template std::future<void> Runtime::spawn(Task<void>);
  template std::future<std::thread::id> Runtime::spawn(Task<std::thread::id>);
} // namespace ao::rt::async
