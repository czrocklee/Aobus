// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Task.h"
#include "TaskFuture.h"
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/utility/ScopedRegistration.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
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
    // A non-null sleeper replaces the default steady-timer sleepFor with an
    // injected delay strategy; the Sleeper must outlive this Runtime.
    explicit Runtime(Executor& callbackExecutor, Sleeper* sleeper = nullptr);
    Runtime(Executor& callbackExecutor, std::size_t workerCount, Sleeper* sleeper = nullptr);
    ~Runtime();

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    Executor& callbackExecutor() noexcept;

    // Terminally closes callback-executor resumption admission and stops the
    // worker pool. The Runtime cannot be restarted afterward.
    void requestStop() noexcept;
    void join();

    Task<void> resumeOnCallbackExecutor(std::stop_token stopToken = {});
    Task<void> resumeOnWorker(std::stop_token stopToken = {});
    Task<void> sleepFor(std::chrono::milliseconds delay, std::stop_token stopToken = {});

    /**
     * Run @p tasks concurrently on this Runtime's worker pool and complete
     * once every task has finished.
     *
     * The awaiting coroutine is suspended while the tasks run and holds no
     * worker thread, so `whenAll` cannot starve the pool: with a pool of one
     * thread the tasks simply run sequentially. If any task exits with an
     * exception, the first one in task order is rethrown after all tasks have
     * completed. Cancellation of the awaiting coroutine is forwarded to the
     * spawned tasks.
     */
    Task<> whenAll(std::vector<Task<>> tasks);

    void spawnLogged(Task<void> task, std::string_view fatalContext = "root coroutine");
    TaskHandle spawnCancellable(CancellableTask task, std::string_view fatalContext = "cancellable coroutine");

    template<typename T>
    TaskFuture<T> spawn(Task<T> task)
    {
      if constexpr (std::is_void_v<T>)
      {
        return TaskFuture<void>{boost::asio::co_spawn(workerPool(), std::move(task), boost::asio::use_future)};
      }
      else
      {
        // Boost.Asio supplies an empty optional to its completion token when
        // the source fails, while use_future preserves the source exception.
        auto transport = [](Task<T> source) -> Task<std::optional<T>>
        { co_return std::optional<T>{co_await std::move(source)}; }(std::move(task));
        return TaskFuture{boost::asio::co_spawn(workerPool(), std::move(transport), boost::asio::use_future)};
      }
    }

    void spawnWithLifetime(LifetimeScope& scope,
                           CancellableTask task,
                           std::string_view fatalContext = "lifetime-bound coroutine");

  private:
    struct CallbackState;
    static void finishFireAndForget(std::exception_ptr exceptionPtr, std::string_view context) noexcept;

    compat::MoveOnlyFunction<void()> startCancellable(CancellableTask task,
                                                      std::function<void(std::exception_ptr)> completion);

    boost::asio::thread_pool& workerPool() noexcept;

    Executor& _callbackExecutor;
    std::shared_ptr<CallbackState> _callbackStatePtr;
    Sleeper* _sleeper;
  };
} // namespace ao::async
