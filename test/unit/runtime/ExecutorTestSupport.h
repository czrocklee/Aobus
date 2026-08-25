// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Executor.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>

namespace ao::async
{
  class LoopExecutor;
}

namespace ao::rt::test
{
  class ManualExecutor final : public async::Executor
  {
  public:
    ManualExecutor();
    ~ManualExecutor() override;

    ManualExecutor(ManualExecutor const&) = delete;
    ManualExecutor& operator=(ManualExecutor const&) = delete;
    ManualExecutor(ManualExecutor&&) = delete;
    ManualExecutor& operator=(ManualExecutor&&) = delete;

    bool isCurrent() const noexcept override;
    void dispatch(compat::MoveOnlyFunction<void()> task) override;
    void defer(compat::MoveOnlyFunction<void()> task) override;
    bool runOne();
    void runUntilIdle();

    template<typename Predicate>
    bool drainUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{2})
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (!predicate())
      {
        if (runOne())
        {
          continue;
        }

        if (auto const now = std::chrono::steady_clock::now();
            now >= deadline || !waitUntilQueued(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)))
        {
          return predicate();
        }
      }

      return true;
    }

    std::size_t queuedCount() const;
    bool waitUntilQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;
    bool waitUntilQueuedCount(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;
    void checkQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  // Test-only executor that collapses dispatch and defer onto its construction
  // thread's stack and rejects work from foreign threads. Its defer is
  // deliberately reentrant and does not model Executor's later-turn contract.
  class InlineExecutor final : public async::Executor
  {
  public:
    InlineExecutor() noexcept;
    ~InlineExecutor() override = default;

    InlineExecutor(InlineExecutor const&) = delete;
    InlineExecutor& operator=(InlineExecutor const&) = delete;
    InlineExecutor(InlineExecutor&&) = delete;
    InlineExecutor& operator=(InlineExecutor&&) = delete;

    bool isCurrent() const noexcept override;
    void dispatch(compat::MoveOnlyFunction<void()> task) override;
    void defer(compat::MoveOnlyFunction<void()> task) override;

  private:
    void execute(compat::MoveOnlyFunction<void()> task) const;

    std::thread::id _ownerThread;
  };

  // Deterministic test adapter over the production loop executor. Dispatch is
  // deliberately queued so tests can inspect state before delivering a turn.
  class QueuedExecutor final : public async::Executor
  {
  public:
    QueuedExecutor();
    ~QueuedExecutor() override;

    QueuedExecutor(QueuedExecutor const&) = delete;
    QueuedExecutor& operator=(QueuedExecutor const&) = delete;
    QueuedExecutor(QueuedExecutor&&) = delete;
    QueuedExecutor& operator=(QueuedExecutor&&) = delete;

    bool isCurrent() const noexcept override;
    void dispatch(compat::MoveOnlyFunction<void()> task) override;
    void defer(compat::MoveOnlyFunction<void()> task) override;
    void drain();

    template<typename Predicate>
    bool drainUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{2})
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (!predicate())
      {
        if (runReadyTurn())
        {
          continue;
        }

        auto const now = std::chrono::steady_clock::now();

        if (now >= deadline)
        {
          return predicate();
        }

        auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto const pollInterval = std::min(remaining, std::chrono::milliseconds{1});

        // The predicate may observe worker-owned state that does not notify
        // this executor, so periodically recheck it while the queue is empty.
        std::ignore = waitUntilQueued(pollInterval);
      }

      return true;
    }

    std::size_t queuedCount() const;
    bool waitUntilQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;
    bool waitUntilQueuedCount(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;
    void checkQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;

  private:
    bool runReadyTurn();
    void enqueue(compat::MoveOnlyFunction<void()> task);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  bool runLoopUntil(async::LoopExecutor& executor,
                    compat::MoveOnlyFunction<bool()> predicate,
                    std::chrono::milliseconds timeout = std::chrono::seconds{5});
} // namespace ao::rt::test
