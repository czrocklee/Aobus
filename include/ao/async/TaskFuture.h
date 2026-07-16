// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Exception.h>

#include <future>
#include <optional>
#include <utility>

namespace ao::async
{
  template<typename T>
  class [[nodiscard]] TaskFuture final
  {
  public:
    explicit TaskFuture(std::future<std::optional<T>> future)
      : _future{std::move(future)}
    {
    }

    ~TaskFuture() = default;

    TaskFuture(TaskFuture const&) = delete;
    TaskFuture& operator=(TaskFuture const&) = delete;
    TaskFuture(TaskFuture&&) noexcept = default;
    TaskFuture& operator=(TaskFuture&&) noexcept = default;

    T get()
    {
      auto optResult = _future.get();

      if (!optResult)
      {
        throwException<Exception>("Task future completed without a result");
      }

      return std::move(*optResult);
    }

  private:
    // The optional payload prevents future implementations from eagerly
    // default-constructing T before the task has produced a value.
    std::future<std::optional<T>> _future;
  };

  template<>
  class [[nodiscard]] TaskFuture<void> final
  {
  public:
    explicit TaskFuture(std::future<void> future)
      : _future{std::move(future)}
    {
    }

    ~TaskFuture() = default;

    TaskFuture(TaskFuture const&) = delete;
    TaskFuture& operator=(TaskFuture const&) = delete;
    TaskFuture(TaskFuture&&) noexcept = default;
    TaskFuture& operator=(TaskFuture&&) noexcept = default;

    void get() { _future.get(); }

  private:
    std::future<void> _future;
  };
} // namespace ao::async
