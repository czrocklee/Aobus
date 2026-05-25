#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Helper definitions to simulate Aobus environment
// --- Mocks for UseStdNumbersCheck ---
extern "C"
{
  void some_c_api(long* ptr, std::int32_t val);
  using c_size_t = unsigned long;
}

namespace gtkmm_mock
{
  struct Widget
  {
    virtual ~Widget() = default;
    virtual void on_draw(std::int32_t val) = 0;
  };
}

namespace ao::async
{
  struct LifetimeScope
  {};

  template<typename F>
  void runOnMainThread(F&& func)
  {
    std::forward<F>(func)();
  }
}

struct Foo
{
  Foo() = default;
  Foo(std::int32_t /*val*/) {}
  Foo(std::int32_t /*val1*/, std::int32_t /*val2*/) {}
};

struct LocalFoo
{
  LocalFoo() = default;
  LocalFoo(std::int32_t /*val*/) {}
};
