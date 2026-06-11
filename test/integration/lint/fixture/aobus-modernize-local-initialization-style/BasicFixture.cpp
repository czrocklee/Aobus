// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestHelpers.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

void testLocalInitialization()
{
  using namespace std::string_literals;
  using namespace std::string_view_literals;

  // POSITIVE
  LocalFoo explicitLocal{10};

  // POSITIVE
  std::string explicitStr("hello");

  // POSITIVE
  std::string_view explicitSv("world");

  // POSITIVE
  std::vector<int> explicitVec(10, 2);

  // POSITIVE
  std::int32_t bracedPrimitive{5};

  // NEGATIVE
  [[maybe_unused]] auto modernVal = std::int32_t{5};
  [[maybe_unused]] auto optStr = "hello"s;
  [[maybe_unused]] auto optSv = "world"sv;
  [[maybe_unused]] auto optVec = std::vector<int>(10, 2);
  [[maybe_unused]] std::int32_t optPrimitive = 5;

  // NEGATIVE
  [[maybe_unused]] std::int32_t* optPointer = nullptr;
}

namespace coro
{
  struct Task
  {
    struct promise_type
    {
      Task get_return_object() { return {}; }
      std::suspend_never initial_suspend() { return {}; }
      std::suspend_never final_suspend() noexcept { return {}; }
      void return_void() {}
      void unhandled_exception() {}
    };
  };

  struct TupleAwaiter
  {
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> /*handle*/) {}
    std::tuple<std::int32_t, std::size_t> await_resume() { return {}; }
  };

  // Clang synthesizes implicit parameter-move VarDecls for coroutine
  // parameters and parks their location on the first coroutine keyword of the
  // body; neither they nor structured bindings have a stylable spelling.
  Task coroutineParamMoves(bool isError, std::int32_t pid)
  {
    // NEGATIVE
    auto [error, count] = co_await TupleAwaiter{};

    if (isError && error == pid && count > 0)
    {
      co_return;
    }
  }
} // namespace coro
