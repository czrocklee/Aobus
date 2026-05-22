#pragma once

#include <string>
#include <vector>

// Helper definitions to simulate Aobus environment
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
