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
  void test()
  {
    // POSITIVE
    ao::Foo const f;
    // POSITIVE
    ao::Foo const cf;
    // NEGATIVE
    library::Track const t;
    // POSITIVE
    ao::library::Track const t2;
    // POSITIVE
    ao::bar();
  }
}

namespace ao::library
{
  void test2()
  {
    // POSITIVE
    ao::library::Track const t;
    // POSITIVE
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
    // POSITIVE
    ao::test::TempDir const t;
  }
}
}
