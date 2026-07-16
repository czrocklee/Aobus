// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/Parallel.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>
#include <utility>
#include <vector>

namespace ao::async
{
  namespace
  {
    Task<> deferTaskStart(Runtime* runtime, Task<> task)
    {
      // A parallel_group initiates deferred operations sequentially. Suspend
      // each child once so all operations start before user code can block an
      // initiating worker thread.
      co_await boost::asio::post(runtime->workerPool(), boost::asio::use_awaitable);
      co_await std::move(task);
    }
  } // namespace

  // GCC's -Wmaybe-uninitialized reports a false positive for the anonymous
  // tuple that use_awaitable materializes inside this coroutine's frame; the
  // diagnostic is attributed to the whole coroutine body, so the suppression
  // must cover the function.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  Task<> whenAll(Runtime* runtime, std::vector<Task<>> tasks)
  {
    if (tasks.empty())
    {
      co_return;
    }

    using SpawnOperation = decltype(boost::asio::co_spawn(
      runtime->workerPool(), deferTaskStart(runtime, std::move(tasks.front())), boost::asio::deferred));
    auto operations = std::vector<SpawnOperation>{};
    operations.reserve(tasks.size());

    for (auto& task : tasks)
    {
      operations.push_back(
        boost::asio::co_spawn(runtime->workerPool(), deferTaskStart(runtime, std::move(task)), boost::asio::deferred));
    }

    auto [completionOrder, exceptions] =
      co_await boost::asio::experimental::make_parallel_group(std::move(operations))
        .async_wait(boost::asio::experimental::wait_for_all{}, boost::asio::use_awaitable);

    for (auto const& exceptionPtr : exceptions)
    {
      if (exceptionPtr)
      {
        std::rethrow_exception(exceptionPtr);
      }
    }
  }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
} // namespace ao::async
