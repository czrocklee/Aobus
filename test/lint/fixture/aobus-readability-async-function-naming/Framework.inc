// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma clang system_header

#include <coroutine>

namespace boost::asio
{
  template<typename T>
  class awaitable
  {
  public:
    struct promise_type
    {
      awaitable get_return_object() noexcept;
      std::suspend_never initial_suspend() noexcept;
      std::suspend_never final_suspend() noexcept;
      void unhandled_exception() noexcept;

      template<typename U>
      void return_value(U&& value) noexcept;
    };
  };

  template<>
  class awaitable<void>
  {
  public:
    struct promise_type
    {
      awaitable get_return_object() noexcept;
      std::suspend_never initial_suspend() noexcept;
      std::suspend_never final_suspend() noexcept;
      void return_void() noexcept;
      void unhandled_exception() noexcept;
    };
  };
} // namespace boost::asio

namespace ao::async
{
  template<typename T = void>
  using Task = boost::asio::awaitable<T>;
} // namespace ao::async

class AsyncFramework
{
public:
  virtual ao::async::Task<void> execute() = 0;
};
