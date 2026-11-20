// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>
#include <ao/async/OperationCancelled.h>

#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
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

      // Move T before publication. Handing the payload to the promise never
      // calls user code, even when T has a blocking or throwing move.
      explicit TaskFuturePayload(T&& value)
        : _valuePtr{std::make_unique<T>(std::move(value))}
      {
      }

      TaskFuturePayload(TaskFuturePayload const&) = delete;
      TaskFuturePayload& operator=(TaskFuturePayload const&) = delete;
      TaskFuturePayload(TaskFuturePayload&&) noexcept = default;
      TaskFuturePayload& operator=(TaskFuturePayload&&) noexcept = default;
      ~TaskFuturePayload() = default;

      bool hasValue() const noexcept { return _valuePtr != nullptr; }
      T take() && { return std::move(*_valuePtr); }

    private:
      std::unique_ptr<T> _valuePtr;
    };

    // Owned by the co_spawn completion handler. The stop registration borrows
    // this stable address; it unregisters before the promise is destroyed.
    template<typename Payload>
    class TaskFutureState final
    {
    public:
      explicit TaskFutureState(std::stop_token const stopToken)
        : _stopToken{stopToken}, _onStop{_stopToken, Cancel{*this}}
      {
      }

      TaskFutureState(TaskFutureState const&) = delete;
      TaskFutureState& operator=(TaskFutureState const&) = delete;
      TaskFutureState(TaskFutureState&&) = delete;
      TaskFutureState& operator=(TaskFutureState&&) = delete;

      // An executor can discard a completion handler without invoking it.
      ~TaskFutureState() { cancel(); }

      std::future<Payload> getFuture() { return _promise.get_future(); }

      template<typename... Args>
      void finish(std::exception_ptr const& exceptionPtr, Args&&... args)
      {
        std::call_once(_settlement,
                       [&]
                       {
                         if (_stopToken.stop_requested())
                         {
                           _promise.set_exception(std::make_exception_ptr(OperationCancelled{}));
                         }
                         else if (exceptionPtr)
                         {
                           _promise.set_exception(exceptionPtr);
                         }
                         else
                         {
                           _promise.set_value(std::forward<Args>(args)...);
                         }
                       });
      }

    private:
      void cancel() noexcept
      {
        std::call_once(_settlement, [this] { _promise.set_exception(std::make_exception_ptr(OperationCancelled{})); });
      }

      struct Cancel final
      {
        TaskFutureState& state;
        void operator()() const noexcept { state.cancel(); }
      };

      std::promise<Payload> _promise;
      std::once_flag _settlement;
      std::stop_token _stopToken;
      std::stop_callback<Cancel> _onStop;
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

    // Waits for the result, not for task exit. OperationCancelled can be reported
    // while the task still uses borrowed state; it does not roll back prior effects.
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

    // As with TaskFuture<T>::get(), cancellation is not a join or a rollback.
    void get() { _future.get(); }

  private:
    std::future<void> _future;
  };

  TaskFuture(std::future<void>) -> TaskFuture<void>;
} // namespace ao::async
