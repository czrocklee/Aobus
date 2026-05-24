#pragma once

#include <string>
#include <vector>

// Helper definitions to simulate Aobus environment
// --- Mocks for UseStdNumbersCheck ---
extern "C"
{
  void some_c_api(long* ptr, int val);
  typedef unsigned long c_size_t;
}

namespace gtkmm_mock
{
  struct Widget
  {
    virtual void on_draw(int x) = 0;
  };
}

namespace ao::async
{
  struct LifetimeScope
  {};

  template<typename F>
  void runOnMainThread(F&& f)
  {
    f();
  }
}

struct Foo
{
  Foo() = default;
  Foo(int) {}
  Foo(int, int) {}
};

struct LocalFoo
{
  LocalFoo() = default;
  LocalFoo(int) {}
};
