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
    ao::Foo f;
    // NEGATIVE
    library::Track t;
    // POSITIVE
    ao::library::Track t2;
    // POSITIVE
    ao::bar();
  }
}

namespace ao::library
{
  void test2()
  {
    // POSITIVE
    ao::library::Track t;
    // POSITIVE
    ao::Foo f;
  }
}

namespace other
{
  void test3()
  {
    // NEGATIVE
    ao::Foo f;
    // NEGATIVE
    ao::library::Track t;
  }
}
