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

#define AOBUS_AUTO auto

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

  // POSITIVE: FIX-TO: [[maybe_unused]] int primitiveAuto = 0;
  [[maybe_unused]] auto primitiveAuto = 0;

  // POSITIVE: FIX-TO: [[maybe_unused]] bool primitiveBoolAuto = true;
  [[maybe_unused]] auto primitiveBoolAuto = true;

  // POSITIVE: FIX-TO: [[maybe_unused]] double primitiveDoubleAuto = 1.5;
  [[maybe_unused]] auto primitiveDoubleAuto = 1.5;

  // POSITIVE: FIX-TO: [[maybe_unused]] int const primitiveConstAuto = 1;
  [[maybe_unused]] auto const primitiveConstAuto = 1;

  // POSITIVE: FIX-TO: [[maybe_unused]] std::int32_t modernVal = 5;
  [[maybe_unused]] auto modernVal = std::int32_t{5};

  // POSITIVE: FIX-TO: [[maybe_unused]] std::int32_t convertedVal = primitiveAuto;
  [[maybe_unused]] auto convertedVal = std::int32_t{primitiveAuto};

  // POSITIVE: FIX-TO: [[maybe_unused]] std::int32_t parenConvertedVal = primitiveAuto;
  [[maybe_unused]] auto parenConvertedVal = std::int32_t(primitiveAuto);

  // NEGATIVE
  [[maybe_unused]] auto const ratioMid = static_cast<double>(primitiveAuto);

  auto positions = std::vector<int>{1, 2};

  // NEGATIVE
  [[maybe_unused]] auto start = positions.front();

  // NEGATIVE
  [[maybe_unused]] auto prev = start;

  // POSITIVE
  [[maybe_unused]] AOBUS_AUTO macroPrimitiveAuto = 2;

  // NEGATIVE
  [[maybe_unused]] auto optStr = "hello"s;
  [[maybe_unused]] auto optSv = "world"sv;
  [[maybe_unused]] auto optVec = std::vector<int>(10, 2);
  [[maybe_unused]] std::int32_t optPrimitive = 5;
  [[maybe_unused]] auto* optAutoPointer = static_cast<std::int32_t*>(nullptr);

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
