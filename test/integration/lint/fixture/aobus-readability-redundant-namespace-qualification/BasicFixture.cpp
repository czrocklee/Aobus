// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace ao
{
  struct Foo
  {};
  void bar();
  namespace library
  {
    struct Track
    {};
  }
}

namespace ao
{
  void test1()
  {
    // POSITIVE: FIX-TO: Foo const f;
    ao::Foo const f;
    // POSITIVE: FIX-TO: Foo const cf;
    ao::Foo const cf;
    // NEGATIVE
    library::Track const t;
    // POSITIVE: FIX-TO: library::Track const t2;
    ao::library::Track const t2;
    // POSITIVE: FIX-TO: bar();
    ao::bar();
  }
}

namespace ao::library
{
  void test2()
  {
    // POSITIVE: FIX-TO: Track const t;
    ao::library::Track const t;
    // POSITIVE: FIX-TO: Foo const f;
    ao::Foo const f;
  }
}

namespace other
{
  void test3()
  {
    // NEGATIVE
    ao::Foo const f;
    // NEGATIVE
    ao::Foo const cf;
    // NEGATIVE
    ao::library::Track const t;
  }
}

namespace ao::test
{
  struct TempDir
  {};
}

namespace ao::rt::test
{
  using namespace ao;

  void test4()
  {
    // NEGATIVE
    ao::test::TempDir const t;
  }
}

namespace ao::main::test
{
  void test5()
  {
    // NEGATIVE
    ao::test::TempDir const t;
  }
}

namespace ao::rt::other
{
  void test6()
  {
    // NEGATIVE
    ao::test::TempDir const t;
  }
}

namespace ao
{
  class BaseException
  {
  public:
    BaseException(int x) {}
  };
}

namespace ao::sub
{
  class DerivedException : public BaseException
  {
  public:
    // NEGATIVE
    using BaseException::BaseException;
  };
}
