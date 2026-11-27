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

namespace ao::async
{
  class LoopExecutor;
}

namespace ao::rt::test
{
  /**
   * Lets this process's 1 ms polling waits last about 1 ms.
   *
   * The test executors below recheck worker-owned state every millisecond.
   * Windows otherwise rounds each such wait up to its 15.6 ms default timer
   * tick, so the first call raises the process's timer resolution until it
   * exits. Elsewhere this does nothing.
   */
  void requestMillisecondTimerResolution() noexcept;

  namespace detail
  {
    template<typename Predicate, typename RunReady, typename WaitForWork>
    bool tryDrainTestExecutorUntil(Predicate& predicate,
                                   RunReady runReady,
                                   WaitForWork waitForWork,
                                   std::chrono::milliseconds const timeout)
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (!predicate())
      {
        bool const ran = runReady();
        auto const now = std::chrono::steady_clock::now();

        if (now >= deadline)
        {
          return predicate();
        }

        if (!ran)
        {
          auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
          // Worker-only completion can change the predicate without notifying this executor.
          std::ignore = waitForWork(std::min(remaining, std::chrono::milliseconds{1}));
        }
      }

      return true;
    }
  } // namespace detail

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
    bool tryRunOne();
    void runUntilIdle();

    template<typename Predicate>
    bool tryDrainUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{10})
    {
      return detail::tryDrainTestExecutorUntil(
        predicate, [this] { return tryRunOne(); }, [this](auto wait) { return tryWaitUntilQueued(wait); }, timeout);
    }

    std::size_t queuedCount() const;
    bool tryWaitUntilQueued(std::chrono::milliseconds timeout = std::chrono::seconds{10}) const;
    bool tryWaitUntilQueuedCount(std::size_t expected,
                                 std::chrono::milliseconds timeout = std::chrono::seconds{10}) const;
    void checkQueued(std::chrono::milliseconds timeout = std::chrono::seconds{10}) const;

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
    bool tryDrainUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{10})
    {
      return detail::tryDrainTestExecutorUntil(
        predicate,
        [this] { return tryRunReadyTurn(); },
        [this](auto wait) { return tryWaitUntilQueued(wait); },
        timeout);
    }

    std::size_t queuedCount() const;
    bool tryWaitUntilQueued(std::chrono::milliseconds timeout = std::chrono::seconds{10}) const;
    bool tryWaitUntilQueuedCount(std::size_t expected,
                                 std::chrono::milliseconds timeout = std::chrono::seconds{10}) const;
    void checkQueued(std::chrono::milliseconds timeout = std::chrono::seconds{10}) const;

  private:
    bool tryRunReadyTurn();
    void enqueue(compat::MoveOnlyFunction<void()> task);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  bool tryRunLoopUntil(async::LoopExecutor& executor,
                       compat::MoveOnlyFunction<bool()> predicate,
                       std::chrono::milliseconds timeout = std::chrono::seconds{10});
} // namespace ao::rt::test
