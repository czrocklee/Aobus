#include "LintAllCheckFixture.h"

#include <bits/basic_string.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
  // Helper definitions to simulate Aobus environment
  namespace ao::async
  {
    struct LifetimeScope
    {};
    template<typename F>
    void runOnMainThread(F&& func)
    {
      func();
    }
  }

  struct Foo final
  {
    Foo() = default;
    Foo(int /*val*/) {}
    Foo(int /*val1*/, int /*val2*/) {}
  };

  struct LocalFoo final
  {
    LocalFoo() = default;
    LocalFoo(int /*val*/) {}
  };

  // ============================================================================
  // 1. CApiGlobalQualificationCheck
  // ============================================================================
  extern "C" void myLocalCFunction()
  {}

  void testCApiQualification()
  {
    ::getpid();         // OK
    myLocalCFunction(); // OK
  }

  // ============================================================================
  // 2. ConcreteFinalCheck
  // ============================================================================
  class UnmarkedConcrete final
  { // Warning: concrete class 'UnmarkedConcrete' should be marked 'final'
  public:
    void doWork() {}
  };

  // ============================================================================
  // 8. LocalInitializationStyleCheck
  // ============================================================================
  void testLocalInitialization()
  {
    using namespace std::string_literals;

    // POSITIVE: Primitive type using brace-initialization
    [[maybe_unused]] int const bracedPrimitive =
      5; // Fixed manually: int bracedPrimitive{5} -> int bracedPrimitive = 5;

    // NEGATIVE: Correct modern initialization
    [[maybe_unused]] int const modernVal = 5;                     // OK
    [[maybe_unused]] auto const optStr = "hello"s;                // OK
    [[maybe_unused]] auto const optVec = std::vector<int>(10, 2); // OK
    [[maybe_unused]] int const optPrimitive = 5;                  // OK
  }

  // ============================================================================
  // 9. BracedInitializationCheck
  // ============================================================================
  class BracedDemoBase
  {
  public:
    BracedDemoBase(int /*val*/) {}
  };

  class BracedDemo final : public BracedDemoBase
  {
    static constexpr int kInitVal1 = 10;
    static constexpr int kInitVal2 = 20;

  public:
    // 9.1 Member/Base Initializers
    BracedDemo()
      : BracedDemoBase{kInitVal1}, _data{kInitVal1}, _intData{kInitVal2} // OK
    {
    }

    BracedDemo(double /*val*/)
      : BracedDemoBase{kInitVal1}, _data{kInitVal1}, _intData{kInitVal2}
    { // Warning: use brace initialization 'BracedDemoBase{...}' instead of parentheses
      // Warning: use brace initialization '_data{...}' instead of parentheses
      // Warning: use brace initialization '_intData{...}' instead of parentheses
    }

    void testCases()
    {
      // 9.2 Temporary Objects / Functional Casts (Class types)
      [[maybe_unused]] auto const s1 = std::string{"foo"}; // OK
      [[maybe_unused]] auto const s2 =
        std::string("bar"); // Warning: use brace initialization 'std::string{...}' instead of parentheses

      // 9.3 New Expressions
      auto* const s3 = new std::string{"baz"}; // OK
      auto* const s4 =
        new std::string("qux"); // Warning: use brace initialization 'std::string{...}' instead of parentheses
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      delete s3;
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      delete s4;

      // 9.4 Local Variables (Non-primitive)
      // NOLINTNEXTLINE(aobus-modernize-local-initialization-style)
      [[maybe_unused]] std::string const s5{"local"}; // OK
      // NOLINTNEXTLINE(aobus-modernize-local-initialization-style)
      [[maybe_unused]] std::string const s6(
        "local"); // Warning: use brace initialization 's6{...}' instead of parentheses

      // 9.5 Safe standard containers (Single non-integer arg)
      [[maybe_unused]] auto const s7 =
        std::string(s1); // Warning: use brace initialization 'std::string{...}' instead of parentheses
      [[maybe_unused]] auto const p1 = std::filesystem::path(
        s1); // Warning: use brace initialization 'std::filesystem::path{...}' instead of parentheses

      // 9.6 DANGEROUS cases (Should be IGNORED)
      [[maybe_unused]] auto const v1 = std::vector<int>(kInitVal1);       // OK (size constructor)
      [[maybe_unused]] auto const v2 = std::vector<int>(kInitVal1, 1);    // OK (size + value)
      [[maybe_unused]] auto const s8 = std::string(kInitVal1, 'a');       // OK (count + char)
      [[maybe_unused]] auto const s9 = std::string(s1.begin(), s1.end()); // OK (iterator range - 2 args)

      // 9.7 Scalar Casts (Should be IGNORED)
      [[maybe_unused]] auto const u1 = std::uintptr_t(&s1); // OK (scalar functional cast)
      [[maybe_unused]] auto const i1 = int(1.5);            // OK (scalar functional cast)
    }

  private:
    Foo _data;
    int _intData;
  };
} // namespace
