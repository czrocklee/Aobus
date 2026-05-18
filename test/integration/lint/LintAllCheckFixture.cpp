#include "LintAllCheckFixture.h"
#include <cmath>
#include <iostream>
#include <mutex>
#include <optional>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// Helper definitions to simulate Aobus environment
namespace ao::async
{
  struct LifetimeScope
  {};
  void runOnMainThread(auto&& f)
  {
    f();
  }
}

struct Foo
{
  Foo() {}
  Foo(int) {}
  Foo(int, int) {}
};

struct LocalFoo
{
  LocalFoo() {}
  LocalFoo(int) {}
};

namespace std
{
  using ::strlen;
}

// ============================================================================
// 1. CApiGlobalQualificationCheck
// ============================================================================
extern "C" void my_local_c_function()
{}

void testCApiQualification()
{
  // POSITIVE: Calling global C API functions without :: qualification
  getpid(); // Warning: external C library function 'getpid' must use global qualification '::getpid'

  // NEGATIVE: Qualified call
  ::getpid(); // OK

  // NEGATIVE: Declared in the project, not system headers
  my_local_c_function(); // OK
}

// ============================================================================
// 2. ConcreteFinalCheck
// ============================================================================
namespace
{
  // POSITIVE: Concrete class in anonymous namespace without 'final'
  class UnmarkedConcrete
  { // Warning: concrete class 'UnmarkedConcrete' should be marked 'final'
  public:
    void doWork() {}
  };

  // POSITIVE: Concrete struct in anonymous namespace without 'final'
  struct UnmarkedStruct
  { // Warning: concrete struct 'UnmarkedStruct' should be marked 'final'
    int val;
  };

  // NEGATIVE: Correctly marked final
  class CorrectConcrete final
  { // OK
  public:
    void doWork() {}
  };

  // NEGATIVE: Designed for inheritance (has protected constructor)
  class DesignedForInheritance
  { // OK (ignored due to protected constructor)
  protected:
    DesignedForInheritance() = default;
  };

  // NEGATIVE: Polymorphic class (has virtual destructor/methods)
  class PolymorphicClass
  {
  public:
    virtual ~PolymorphicClass() = default;
    virtual void doSomething() = 0;
  };

  // NEGATIVE: Derived class (inherits from another class)
  class DerivedClass : public DesignedForInheritance
  {
  public:
    void run() {}
  };

  // NEGATIVE: Interface name (starts with I followed by uppercase)
  class IProcessor
  {
  public:
    void process() {}
  };

  // NEGATIVE: Base class name (ends with Base)
  class ProcessorBase
  {
  public:
    void process() {}
  };

  // NEGATIVE: Protected destructor
  class ProtectedDtorClass
  {
  public:
    void run() {}

  protected:
    ~ProtectedDtorClass() = default;
  };

  // Class nesting checks
  class OuterClass
  {
  private:
    // POSITIVE: Concrete private nested class
    class PrivateNested
    { // Warning: concrete class 'PrivateNested' should be marked 'final'
    public:
      void run() {}
    };

  public:
    // NEGATIVE: public nested class (exempt)
    class PublicNested
    {
    public:
      void run() {}
    };
  };
}

// ============================================================================
// 3. ControlBlockSpacingCheck
// ============================================================================
void testControlBlockSpacing(int x)
{
  // NEGATIVE: Correctly spaced control statement (if)
  if (x > 0)
  { // OK
    x++;
  }

  // NEGATIVE: Correctly spaced control statement (for)
  for (int i = 0; i < 10; ++i)
  { // OK
    x += i;
  }

  // NEGATIVE: Correctly spaced control statement (while)
  while (x > 100)
  { // OK
    x--;
  }

  // NEGATIVE: Correctly spaced control statement (switch)
  switch (x)
  { // OK
    default: break;
  }

  // NEGATIVE: Perfect spacing
  if (x > 0)
  { // OK
    x++;
  }

  // NEGATIVE: Spacing with else (intermediate brace)
  if (x > 0)
  {
    x++;
  }
  else if (x < -10)
  {
    x--;
  }

  // POSITIVE: Spacing after block closure missing blank line
  if (x > 0)
  {
    x++;
  }
  x--; // Warning: expected a blank line after closing '}' of control block

  // POSITIVE: Missing blank line before if
  x++;
  if (x > 0)
  { // Warning: expected a blank line before 'if'
    x++;
  }

  // POSITIVE: Missing blank line before for
  x++;
  for (int i = 0; i < 10; ++i)
  { // Warning: expected a blank line before 'for'
    x += i;
  }

  // POSITIVE: Missing blank line before while
  x++;
  while (x > 100)
  { // Warning: expected a blank line before 'while'
    x--;
  }

  // POSITIVE: Missing blank line before switch
  x++;
  switch (x)
  { // Warning: expected a blank line before 'switch'
    default: break;
  }

  // POSITIVE: Missing blank line before do-while
  x++;
  do
  { // Warning: expected a blank line before 'do'
    x--;
  }
  while (x > 0);

  // POSITIVE: Missing blank line after for block closure }
  for (int i = 0; i < 10; ++i)
  {
    x += i;
  }
  x++; // Warning: expected a blank line after closing '}' of control block

  // POSITIVE: Missing blank line after while block closure }
  while (x > 100)
  {
    x--;
  }
  x++; // Warning: expected a blank line after closing '}' of control block

  // POSITIVE: Missing blank line after switch block closure }
  switch (x)
  {
    default: break;
  }
  x++; // Warning: expected a blank line after closing '}' of control block

  // POSITIVE: Missing blank line after do-while block closure }
  do
  {
    x--;
  }
  while (x > 0);
  x++; // Warning: expected a blank line after closing '}' of control block
}

void testControlBlockCommentSpacing(int x)
{
  // POSITIVE: Comment separated by a blank line
  // This is some description

  if (x > 0)
  { // Warning: comment should be directly above 'if' without a blank line
    x++;
  }

  // NEGATIVE: Comment directly above control statement (correct)
  // This is some description
  if (x > 0)
  { // OK
    x++;
  }
}

void testControlBlockTryCatchSpacing(int x)
{
  // POSITIVE: Missing blank line before try keyword
  x++;
  try
  { // Warning: expected a blank line before 'try'
    if (x > 0)
    {
      throw 42;
    }
  }
  catch (int)
  {
    x = 0;
  }

  // POSITIVE: Missing blank line after catch block
  try
  {
    throw 42;
  }
  catch (int)
  {
    x = 1;
  }
  x--; // Warning: expected a blank line after closing '}' of control block

  // NEGATIVE: Perfectly spaced try-catch blocks (with a blank line before/after)

  try
  {
    throw 42;
  }
  catch (int)
  {
    x = 2;
  }

  // OK: No warning on intermediate closing brace followed by catch
  try
  {
    throw 42;
  }
  catch (int)
  {
    x = 3;
  }
  catch (...)
  {
    x = 4;
  }
}

// ============================================================================
// 4. ForbidNodiscardCheck
// ============================================================================
// POSITIVE: Using [[nodiscard]] on non-conforming or forbidden patterns
[[nodiscard]] int getForbiddenVal()
{ // Warning: remove [[nodiscard]] from 'getForbiddenVal'
  return 42;
}

// POSITIVE: Using [[nodiscard]] on class definition
struct [[nodiscard]] ForbiddenStruct
{}; // Warning: remove [[nodiscard]] from 'ForbiddenStruct'
class [[nodiscard]] ForbiddenClass
{}; // Warning: remove [[nodiscard]] from 'ForbiddenClass'

// NEGATIVE: Correct location or omitting attribute
int getConformingVal()
{ // OK
  return 42;
}

// ============================================================================
// 5. ForbidTrailingReturnCheck
// ============================================================================
// POSITIVE: Using trailing returns for simple, non-deduced types
auto testTrailing() -> int
{ // Warning: non-lambda function 'testTrailing' uses trailing return type
  return 10;
}

// POSITIVE/NEGATIVE: All non-lambda functions using trailing return will warn
auto testNecessaryTrailing() -> decltype(auto)
{ // Warning: non-lambda function 'testNecessaryTrailing' uses trailing return type
  static int x = 5;
  return (x);
}

// NEGATIVE: Lambda with trailing return (exempt)
auto lambdaWithTrailing = [](int x) -> double { return x * 1.0; }; // OK

// NEGATIVE: Deduction guide (exempt)
template<typename T>
struct DeductionDemo
{
  DeductionDemo(T) {}
};
DeductionDemo(int) -> DeductionDemo<double>; // OK

// ============================================================================
// 6. IdentifierNamingExtensionsCheck
// ============================================================================
class NamingDemo
{
public:
  // POSITIVE: Class member missing the leading underscore prefix
  int memberValue = 10; // Warning: class data member 'memberValue' must use _camelCase (underscore prefix)

  // POSITIVE: Class member starting with _ but snake_case
  int _member_invalid = 5; // Warning: class data member '_member_invalid' should be _camelCase after the underscore

  // POSITIVE: Class member starting with _ but TitleCase
  int _InvalidTitle = 6; // Warning: class data member '_InvalidTitle' should be _camelCase after the underscore

  // NEGATIVE: Conforming class member naming
  int _conformingValue = 20; // OK
};

struct StructNamingDemo
{
  // POSITIVE: Struct member using underscore prefix (violates camelCase struct naming)
  int _invalidStructVal =
    5; // Warning: struct data member '_invalidStructVal' should use camelCase without underscore prefix

  // NEGATIVE: Struct camelCase naming
  int validStructVal = 10; // OK
};

struct ClassLikeStruct
{
private:
  int _privateVal = 0; // NEGATIVE: Exempt due to private member (considered class-like)
};

// ============================================================================
// 7. LambdaParamsCheck
// ============================================================================
void testLambdaParams()
{
  // POSITIVE: Lambda with empty parenthesized parameter list (should be omitted)
  auto invalidLambda = []() { std::cout << "Hi"; }; // Warning: omit empty parameter list '()' in lambda

  // NEGATIVE: Explicit param list omitted when empty
  auto validLambda = [] { std::cout << "Hi"; }; // OK

  // NEGATIVE: Parameter list with parameters
  auto paramsLambda = [](int x) { std::cout << x; }; // OK
}

// ============================================================================
// 8. LocalInitializationStyleCheck
// ============================================================================
void testLocalInitialization()
{
  using namespace std::string_literals;
  using namespace std::string_view_literals;

  // POSITIVE: Explicit type declaration for class type
  LocalFoo explicitLocal{10}; // Warning: use 'auto explicitLocal = Type{...}' instead of explicit type initialization

  // POSITIVE: String/StringView constructed explicitly from string literal
  std::string explicitStr(
    "hello"); // Warning: prefer standard literals 'auto explicitStr = "..."s' over explicit string construction
  std::string_view explicitSv(
    "world"); // Warning: prefer standard literals 'auto explicitSv = "..."sv' over explicit string construction

  // POSITIVE: STL Container initialized using explicit type and non-list initializer
  std::vector<int> explicitVec(
    10, 2); // Warning: use 'auto explicitVec = Type(...)' for container initialization to avoid ambiguity

  // POSITIVE: Primitive type using brace-initialization
  int bracedPrimitive{5}; // Warning: primitive type should use assignment-style initialization 'Type bracedPrimitive =
                          // ...', not brace initialization

  // NEGATIVE: Correct modern initialization
  auto modernVal = int{5};               // OK
  auto optStr = "hello"s;                // OK
  auto optSv = "world"sv;                // OK
  auto optVec = std::vector<int>(10, 2); // OK
  int optPrimitive = 5;                  // OK

  // NEGATIVE: Pointer type initialization
  int* optPointer = nullptr; // OK
}

// ============================================================================
// 9. MemberInitializerBracesCheck
// ============================================================================
class BracesDemo
{
public:
  // POSITIVE: Member initializers using parentheses instead of uniform braces for class types
  BracesDemo()
    : _data(10)
  {
  } // Warning: member initializer should use brace initialization '_data{...}'

  // NEGATIVE: Clean uniform initialization
  BracesDemo(int)
    : _data{10}
  {
  } // OK
private:
  Foo _data;
};

struct ZeroArgDemo
{
  ZeroArgDemo()
    : _foo()
  {
  } // NEGATIVE: zero arguments (exempt)
  Foo _foo;
};

struct MultiArgDemo
{
  MultiArgDemo()
    : _bar(1, 2)
  {
  } // NEGATIVE: multi-argument parenthesized constructor (exempt)
  Foo _bar;
};

// ============================================================================
// 11. OptionalNamingAndUsageCheck
// ============================================================================
void testOptionalUsage(std::optional<int> optVal)
{
  // POSITIVE: std::optional variable missing 'opt' prefix
  std::optional<int> invalidOptName =
    42; // Warning: std::optional variable 'invalidOptName' must have 'opt' prefix (e.g., 'optInvalidOptName')

  // NEGATIVE: Correctly named optional
  auto optValidName = std::optional<int>{42}; // OK

  // POSITIVE: Using has_value() instead of bool cast
  if (optVal.has_value())
  { // Warning: prefer concise 'if (opt)' or 'if (!opt)' over '.has_value()'
    int val = *optVal;
  }

  // NEGATIVE: Safe optional dereference using direct context check
  if (optVal)
  {
    int safeVal = *optVal; // OK
  }
}

// ============================================================================
// 12. StdCLibraryQualificationCheck
// ============================================================================
void testStdCQualification()
{
  // POSITIVE: Using C standard library functions without 'std::' prefix
  size_t len =
    strlen("Hello");    // Warning: C standard library function 'strlen' should use 'std::strlen' via <c...> header
  double d = sin(3.14); // Warning: C standard library function 'sin' should use 'std::sin' via <c...> header

  // NEGATIVE: Conforming qualified call
  std::size_t len2 = std::strlen("Hello"); // OK
  double d2 = std::sin(3.14);              // OK
}

// ============================================================================
// 13. ThreadingPolicyCheck
// ============================================================================
void testThreadingPolicy()
{
  // POSITIVE: Spawning raw threads or running code asynchronously outside a LifetimeScope
  std::thread t([] {}); // Warning: prefer std::jthread over std::thread (Rule 4.4.2)
  t.join();

  // NEGATIVE: jthread usage (allowed under Rule 4.4.2)
  std::jthread jt([] {}); // OK

  // POSITIVE: Volatile variable declaration (Rule 4.4.4)
  int volatile volatileVar =
    0; // Warning: 'volatileVar' is volatile; use std::atomic<> for inter-thread communication instead (Rule 4.4.4)

  // Redundant locks vs Essential locks
  std::mutex m;

  // POSITIVE: unique_lock where scoped_lock is preferred (Rule 4.4.3)
  std::unique_lock<std::mutex> redundantLock(
    m); // Warning: prefer std::scoped_lock over std::unique_lock for 'redundantLock' unless you need deferred locking,
        // early unlock, or condition_variable integration (Rule 4.4.3)

  // NEGATIVE: Has defer_lock (essential lock)
  std::unique_lock<std::mutex> deferLock(m, std::defer_lock); // OK

  // NEGATIVE: Lock that undergoes manual unlock (essential lock)
  std::unique_lock<std::mutex> manualLock(m);
  manualLock.unlock(); // OK
}

// ============================================================================
// 14. UnusedSuppressionStyleCheck
// ============================================================================
void testUnusedSuppression(int param)
{
  // POSITIVE: Using void casts to suppress unused variable/parameter warnings
  (void)param; // Warning: void cast '(void)expr' suppresses unused warnings; use [[maybe_unused]] on the declaration or
               // an anonymous parameter instead

  // POSITIVE: Using static_cast to void
  static_cast<void>(param); // Warning: void cast 'static_cast<void>(expr)' suppresses unused warnings; use
                            // [[maybe_unused]] on the declaration or an anonymous parameter instead

  // NEGATIVE: Conforming C++ attribute suppression
  [[maybe_unused]] int param2 = 10; // OK
}

// ============================================================================
// 15. UseIfInitStatementCheck
// ============================================================================
void testUseIfInit(int cond)
{
  // POSITIVE: Local variable only used inside an if block, not merged into init statement
  // Warning: variable 'localX' is only used inside the following 'if' statement; move its
  // declaration into the init-statement
  int localX = cond * 2;

  if (localX > 10)
  {
    std::cout << localX;
  }

  // POSITIVE: Local variable only used inside switch block
  // Warning: variable 'localSwitch' is only used inside the following 'switch' statement;
  // move its declaration into the init-statement
  int localSwitch = cond + 1;

  switch (localSwitch)
  {
    case 1: break;
    default: break;
  }

  // NEGATIVE: Variable used after target control statement
  int usedAfter = cond * 3;

  if (usedAfter > 0)
  {
    std::cout << usedAfter;
  }

  std::cout << usedAfter; // OK

  // NEGATIVE: constexpr or static variable
  constexpr int constVar = 42;

  if (constVar > 0)
  {
    std::cout << constVar;
  } // OK

  // NEGATIVE: Already merged
  if (int localY = cond * 2; localY > 10)
  { // OK
    std::cout << localY;
  }
}
