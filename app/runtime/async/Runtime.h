// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Task.h"
#include <boost/asio/thread_pool.hpp>
#include <future>
#include <runtime/CorePrimitives.h>

namespace ao::rt::async
{
  class Runtime final
  {
  public:
    explicit Runtime(rt::IControlExecutor& uiExecutor);
    ~Runtime();

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    rt::IControlExecutor& uiExecutor() noexcept;

    void requestStop() noexcept;
    void join();

    boost::asio::thread_pool& workerPool() noexcept;

  private:
    rt::IControlExecutor& _uiExecutor;
    boost::asio::thread_pool _workerPool;
    bool _stopRequested{false};
  };

  // Awaitable to resume execution on the UI control thread
  Task<void> resumeOnUi(Runtime& runtime);

  // Awaitable to resume execution on the background worker pool
  Task<void> resumeOnWorker(Runtime& runtime);

  // Spawn a root task with exception logging
  void spawnLogged(Runtime& runtime, Task<void> task);

  template<typename T>
  std::future<T> spawn(Runtime& runtime, Task<T> task);
} // namespace ao::rt::async
