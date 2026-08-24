// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/Runtime.h>

#include <ao/Contract.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Sleeper.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace ao::async
{
  struct Runtime::CallbackState final
  {
    explicit CallbackState(std::size_t const workerCount)
      : workerPool{workerCount}
    {
    }

    std::atomic_bool acceptsCallbacks{true};
    boost::asio::thread_pool workerPool;
  };

  namespace
  {
    template<typename State, typename Handler>
    struct CallbackDispatch final
    {
      void operator()()
      {
        if (statePtr->acceptsCallbacks.load(std::memory_order_acquire))
        {
          std::move(handler)();
        }
        // Dropping the Asio awaitable handler is the cancellation path after
        // Runtime teardown. Its destructor unwinds the suspended coroutine on
        // the handler executor; statePtr keeps that worker pool alive through
        // this object's reverse member destruction.
      }

      // Destruction is reversed, so the handler releases its Asio executor
      // before the state releases the worker pool.
      std::shared_ptr<State> statePtr;
      Handler handler;
    };

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

  void Runtime::finishFireAndForget(std::exception_ptr exceptionPtr, std::string_view const context) noexcept
  {
    if (!exceptionPtr || isOperationCancelled(exceptionPtr))
    {
      return;
    }

    AO_FATAL_EXCEPTION(std::move(exceptionPtr), context);
  }

  Runtime::Runtime(Executor& callbackExecutor, Sleeper* sleeper)
    : Runtime{callbackExecutor, std::max(1U, std::thread::hardware_concurrency()), sleeper}
  {
  }

  Runtime::Runtime(Executor& callbackExecutor, std::size_t workerCount, Sleeper* sleeper)
    : _callbackExecutor{callbackExecutor}
    , _callbackStatePtr{std::make_shared<CallbackState>(workerCount)}
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

  void Runtime::requestStop() noexcept
  {
    // Closing callback admission is part of the terminal stop boundary, not
    // deferred until member destruction. Composite owners call requestStop()
    // while their callback targets are still alive.
    _callbackStatePtr->acceptsCallbacks.store(false, std::memory_order_release);
    _callbackStatePtr->workerPool.stop();
  }

  void Runtime::join()
  {
    _callbackStatePtr->workerPool.join();
  }

  boost::asio::thread_pool& Runtime::workerPool() noexcept
  {
    return _callbackStatePtr->workerPool;
  }

  Task<void> Runtime::resumeOnCallbackExecutor(std::stop_token const stopToken)
  {
    throwIfStopRequested(stopToken);

    try
    {
      co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void()>(
        [this](auto handler)
        {
          using Dispatch = CallbackDispatch<CallbackState, std::decay_t<decltype(handler)>>;
          callbackExecutor().dispatch(Dispatch{_callbackStatePtr, std::move(handler)});
        },
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
    AO_EXPECTS(delay > std::chrono::milliseconds::zero());
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

  void Runtime::spawnLogged(Task<void> task, std::string_view const fatalContext)
  {
    boost::asio::co_spawn(workerPool(),
                          std::move(task),
                          [fatalContext = std::string{fatalContext}](std::exception_ptr exceptionPtr)
                          { finishFireAndForget(std::move(exceptionPtr), fatalContext); });
  }

  compat::MoveOnlyFunction<void()> Runtime::startCancellable(CancellableTask task,
                                                             std::function<void(std::exception_ptr)> completion)
  {
    auto stopSourcePtr = std::make_shared<std::stop_source>();
    boost::asio::co_spawn(workerPool(),
                          runCancellable(std::move(task), stopSourcePtr->get_token()),
                          [completion = std::move(completion)](std::exception_ptr exPtr) { completion(exPtr); });
    return [stopSourcePtr] { std::ignore = stopSourcePtr->request_stop(); };
  }

  TaskHandle Runtime::spawnCancellable(CancellableTask task, std::string_view const fatalContext)
  {
    return TaskHandle{startCancellable(std::move(task),
                                       [fatalContext = std::string{fatalContext}](std::exception_ptr exceptionPtr)
                                       { finishFireAndForget(std::move(exceptionPtr), fatalContext); })};
  }
} // namespace ao::async
