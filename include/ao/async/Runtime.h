// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Task.h"
#include "TaskFuture.h"
#include <ao/utility/ScopedRegistration.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/system_executor.hpp>
#include <boost/asio/thread_pool.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::async
{
  class Executor;
  class LifetimeScope;
  class Sleeper;

  using TaskHandle = utility::ScopedRegistration;

  class Runtime final
  {
  public:
    // A non-null sleeper replaces the default steady-timer sleepForAsync with an
    // injected delay strategy; the Sleeper must outlive this Runtime.
    explicit Runtime(Executor& callbackExecutor, Sleeper* sleeper = nullptr);
    Runtime(Executor& callbackExecutor, std::size_t workerCount, Sleeper* sleeper = nullptr);
    ~Runtime();

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    Executor& callbackExecutor() noexcept;
    std::size_t workerCount() const noexcept;

    // Terminally closes callback-executor resumption admission and stops the
    // worker pool. Outstanding spawn() futures are settled before return.
    void requestStop() noexcept;
    // Without a preceding stop, let work finish normally. Joining waits for
    // worker threads, not destruction of abandoned coroutine frames.
    void join();

    Task<void> resumeOnCallbackExecutorAsync(std::stop_token stopToken = {});
    // Posts a new worker turn even if the caller is already on a worker.
    Task<void> resumeOnWorkerAsync(std::stop_token stopToken = {});
    Task<void> sleepForAsync(std::chrono::milliseconds delay, std::stop_token stopToken = {});

    /**
     * Run @p tasks concurrently on this Runtime's worker pool and complete
     * once every task has finished.
     *
     * The awaiting coroutine is suspended while the tasks run and holds no
     * worker thread. Each child has a queued start, not an inline call into the
     * child from task submission. With one worker, tasks can interleave only at
     * suspension points. If any task exits with an exception, the first one in
     * task order is rethrown after all tasks have completed. Cancellation of the
     * awaiting coroutine is forwarded to the spawned tasks.
     */
    Task<> whenAllAsync(std::vector<Task<>> tasks);

    void spawnLogged(Task<void> task, std::string_view fatalContext = "root coroutine");
    TaskHandle spawnCancellable(CancellableTask task, std::string_view fatalContext = "cancellable coroutine");

    // Stop settles pending futures without waiting for their tasks or frames.
    template<typename T>
    TaskFuture<T> spawn(Task<T> task)
    {
      using Payload = std::conditional_t<std::is_void_v<T>, void, detail::TaskFuturePayload<T>>;
      auto const stopToken = _futureStopSource.get_token();
      auto statePtr = std::make_unique<detail::TaskFutureState<Payload>>(stopToken);
      auto future = TaskFuture{statePtr->getFuture()};

      if (stopToken.stop_requested())
      {
        return future;
      }

      // Dispatch settlement inline on the finishing turn, including a callback
      // turn. Reading an already finished task must not need a free worker.
      auto complete = boost::asio::bind_executor(
        boost::asio::system_executor{},
        [statePtr = std::move(statePtr)](std::exception_ptr const& exceptionPtr, auto&&... payload)
        { statePtr->finish(exceptionPtr, std::forward<decltype(payload)>(payload)...); });

      if constexpr (std::is_void_v<T>)
      {
        boost::asio::co_spawn(workerPool(), std::move(task), std::move(complete));
      }
      else
      {
        // Asio requires a default payload on failure; T need not have one.
        auto transport = [](Task<T> source) -> Task<Payload>
        { co_return Payload{co_await std::move(source)}; }(std::move(task));
        boost::asio::co_spawn(workerPool(), std::move(transport), std::move(complete));
      }

      return future;
    }

    void spawnWithLifetime(LifetimeScope& scope,
                           CancellableTask task,
                           std::string_view fatalContext = "lifetime-bound coroutine");

  private:
    struct CallbackState;
    static void finishFireAndForget(std::exception_ptr exceptionPtr, std::string_view context) noexcept;

    void startCancellable(CancellableTask task,
                          std::stop_token stopToken,
                          std::function<void(std::exception_ptr)> completion);

    boost::asio::thread_pool& workerPool() noexcept;
    void closeTaskFutures() noexcept;

    Executor& _callbackExecutor;
    std::shared_ptr<CallbackState> _callbackStatePtr;
    Sleeper* _sleeper;
    std::stop_source _futureStopSource;
    std::once_flag _closeTaskFuturesOnce;
  };
} // namespace ao::async
