// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>

#include <future>
#include <optional>
#include <type_traits>
#include <utility>

namespace ao::async
{
  namespace detail
  {
    template<typename T>
    class TaskFuturePayload final
    {
    public:
      TaskFuturePayload() = default;

      explicit TaskFuturePayload(T value)
        : _optValue{std::move(value)}
      {
      }

      TaskFuturePayload(TaskFuturePayload const&) = delete;
      TaskFuturePayload& operator=(TaskFuturePayload const&) = delete;
      TaskFuturePayload(TaskFuturePayload&&) noexcept = default;
      TaskFuturePayload& operator=(TaskFuturePayload&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
      {
        if (this != &other)
        {
          _optValue.reset();

          if (other._optValue)
          {
            _optValue.emplace(std::move(*other._optValue));
          }
        }

        return *this;
      }
      ~TaskFuturePayload() = default;

      bool hasValue() const noexcept { return _optValue.has_value(); }
      T take() && { return std::move(*_optValue); }

    private:
      std::optional<T> _optValue{};
    };
  } // namespace detail

  template<typename T>
  class [[nodiscard]] TaskFuture final
  {
  public:
    explicit TaskFuture(std::future<detail::TaskFuturePayload<T>> future)
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
      auto result = _future.get();

      AO_INVARIANT(result.hasValue(), "Task future completed without a result");

      return std::move(result).take();
    }

  private:
    std::future<detail::TaskFuturePayload<T>> _future;
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
