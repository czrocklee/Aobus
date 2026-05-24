// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Task.h"
#include <ao/rt/CorePrimitives.h>

#include <boost/asio/thread_pool.hpp>

#include <exception>
#include <functional>
#include <future>

namespace ao::rt::async
{
  class LifetimeScope;

  class Runtime final
  {
  public:
    explicit Runtime(rt::IControlExecutor& uiExecutor);
    ~Runtime();

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    rt::IControlExecutor& controlExecutor() noexcept;

    void requestStop() noexcept;
    void join();

    boost::asio::thread_pool& workerPool() noexcept;

    // Awaitable to resume execution on the UI control thread
    Task<void> resumeOnControl();

    // Awaitable to resume execution on the background worker pool
    Task<void> resumeOnWorker();

    // Spawn a root task with exception logging
    void spawnLogged(Task<void> task);

    // Spawn a task with a specific cancellation slot and completion callback
    void spawn(Task<void> task, CancellationSlot slot, std::function<void(std::exception_ptr)> callback);

    template<typename T>
    std::future<T> spawn(Task<T> task);

    void spawnWithLifetime(LifetimeScope* scope, Task<void> task);

  private:
    rt::IControlExecutor& _uiExecutor;
    boost::asio::thread_pool _workerPool;
    bool _stopRequested{false};
  };
} // namespace ao::rt::async
