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

#define FIXTURE_AUTO auto

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

  // POSITIVE
  [[maybe_unused]] auto primitiveAuto = 0;

  // POSITIVE
  [[maybe_unused]] auto primitiveBoolAuto = true;

  // POSITIVE
  [[maybe_unused]] auto primitiveDoubleAuto = 1.5;

  // POSITIVE
  [[maybe_unused]] auto const primitiveConstAuto = 1;

  // POSITIVE
  [[maybe_unused]] auto primitiveCharAuto = 'a';

  // POSITIVE
  [[maybe_unused]] auto primitiveUnaryAuto = -(5);

  // POSITIVE
  [[maybe_unused]] auto primitiveUnaryCharAuto = +('a');

  // POSITIVE
  [[maybe_unused]] auto primitiveParenAuto = ((true));

  // POSITIVE
  [[maybe_unused]] auto modernVal = std::int32_t{5};

  // POSITIVE
  [[maybe_unused]] auto convertedVal = std::int32_t{primitiveAuto};

  // POSITIVE
  [[maybe_unused]] auto parenConvertedVal = std::int32_t(primitiveAuto);

  // POSITIVE
  [[maybe_unused]] auto parenthesizedCast = (std::int32_t{5});

  // POSITIVE
  [[maybe_unused]] auto listLiteral{3};

  // POSITIVE
  [[maybe_unused]] auto listBool{true};

  // POSITIVE
  [[maybe_unused]] auto listFloat{0.5f};

  // POSITIVE
  [[maybe_unused]] auto listChar{'a'};

  // POSITIVE
  [[maybe_unused]] auto listUnary{-('a')};

  // POSITIVE
  [[maybe_unused]] auto listParen{((false))};

  // POSITIVE
  [[maybe_unused]] auto listFunctionalCast{std::int32_t(7)};

  // NEGATIVE
  [[maybe_unused]] auto listStaticCast{static_cast<std::int32_t>(7)};

  // NEGATIVE
  [[maybe_unused]] auto localCastProduct = std::size_t{8} * 1024;

  // NEGATIVE
  [[maybe_unused]] auto const ratioMid = static_cast<double>(primitiveAuto);

  auto positions = std::vector<int>{1, 2};

  // NEGATIVE
  [[maybe_unused]] auto listFunctionResult{positions.size()};

  // NEGATIVE
  [[maybe_unused]] auto listCastProduct{std::size_t{8} * 1024};

  // NEGATIVE
  [[maybe_unused]] auto start = positions.front();

  // NEGATIVE
  [[maybe_unused]] auto prev = start;

  // POSITIVE
  [[maybe_unused]] FIXTURE_AUTO macroPrimitiveAuto = 2;

  // NEGATIVE
  [[maybe_unused]] auto optStr = "hello"s;
  [[maybe_unused]] auto optSv = "world"sv;
  [[maybe_unused]] auto optVec = std::vector<int>(10, 2);
  [[maybe_unused]] std::int32_t optPrimitive = 5;
  [[maybe_unused]] auto* optAutoPointer = static_cast<std::int32_t*>(nullptr);

  // NEGATIVE
  [[maybe_unused]] std::int32_t* optPointer = nullptr;
}

namespace
{
  enum Axis
  {
    Horizontal,
  };

  constexpr Axis pickAxis()
  {
    return Horizontal;
  }

  // NEGATIVE
  [[maybe_unused]] constexpr auto kNamespaceAxis = Axis{Horizontal};

  struct Palette
  {
    // NEGATIVE
    static constexpr Axis kAxis{Horizontal};
  };
} // namespace

void testEnumeratorInitialization()
{
  enum class Color
  {
    Red,
  };
  enum Flag : unsigned
  {
  };
  using Alias = Color;
  using VolatileColor = Color volatile;
  using enum Color;
  struct Swatch
  {
    Color color;
    operator Color() const { return color; }
  };

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] auto color = Color{Color::Red};

  // NEGATIVE
  [[maybe_unused]] auto direct = Color::Red;

  // NEGATIVE
  [[maybe_unused]] Color typed = Color::Red;

  // NEGATIVE
  [[maybe_unused]] auto deduced(Color::Red);

  // NEGATIVE
  [[maybe_unused]] auto deducedList{Color::Red};

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] auto wrappedList{Color{Color::Red}};

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] auto nested = Color{Color{Color::Red}};

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] auto nestedParens = Color(Color(Color::Red));

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] Color const nestedTyped = {Color{Color::Red}};

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] constexpr Color nestedConstant(Color{Color::Red});

  // NEGATIVE
  [[maybe_unused]] Color const directConstant = Color::Red;

  // NEGATIVE
  [[maybe_unused]] auto nestedConversion = Color{Color(Horizontal)};

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] auto const aliased = Alias(Red);

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] constexpr auto axis = Axis{Horizontal};

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] Color listed{Color::Red};

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] Color copyListed = {Color::Red};

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] Color parenthesized(Color::Red);

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] Color grouped((Color::Red));

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] Color copyWrapped = Color{Color::Red};

  // POSITIVE | redundant initialization from a same-type enumerator
  for (Color index{Color::Red}; index == Color::Red;)
  {
    break;
  }

  // POSITIVE | redundant initialization from a same-type enumerator
  if (Axis axisCondition = Axis{Horizontal})
  {
    (void)axisCondition;
  }

  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] Axis parenthesizedAxis(Horizontal);

  // NEGATIVE
  [[maybe_unused]] Axis selected(pickAxis());

  // POSITIVE | for a braced enum local, prefer auto with the enum type on the initializer
  [[maybe_unused]] Axis bracedResult{pickAxis()};

  // POSITIVE | for a braced enum local, prefer auto with the enum type on the initializer
  [[maybe_unused]] Color bracedCopy{direct};

  // NEGATIVE
  [[maybe_unused]] VolatileColor polled{Color::Red};

  enum
  {
    Ready,
  } anonymous{Ready}; // NEGATIVE

  enum class State
  {
    Idle,
  } state{State::Idle}; // NEGATIVE

  // NEGATIVE
  [[maybe_unused]] Color first{Color::Red}, *next = nullptr;

  // NEGATIVE
  [[maybe_unused]] Flag uninitialized;

  // NEGATIVE
  [[maybe_unused]] Color uninitializedScoped;

  // NEGATIVE
  [[maybe_unused]] std::byte uninitializedByte;

  // NEGATIVE
  [[maybe_unused]] auto number = Flag{1u};

  // NEGATIVE
  [[maybe_unused]] auto mask = std::byte{0x01};

  // POSITIVE | for a braced enum local, prefer auto with the enum type on the initializer
  [[maybe_unused]] constexpr std::byte typedMask{0xff};

  // POSITIVE | for a braced enum local, prefer auto with the enum type on the initializer
  [[maybe_unused]] Color empty{};

  // NEGATIVE
  [[maybe_unused]] auto converted = Color(Horizontal);

  // NEGATIVE
  [[maybe_unused]] auto swatch = Swatch{Color::Red};

  // NEGATIVE
  [[maybe_unused]] Color fromProxy(swatch);

  // NEGATIVE
  [[maybe_unused]] auto copied = Color{direct};

  // NEGATIVE
  [[maybe_unused]] auto cast = static_cast<Color>(Color::Red);

  // NEGATIVE
  [[maybe_unused]] auto const& reference = Color{Color::Red};

  // NEGATIVE
  [[maybe_unused]] Color const& bound(Color{Color::Red});

  // NEGATIVE
  [[maybe_unused]] decltype(auto) preserved = Color{Color::Red};

  // NEGATIVE
  [[maybe_unused]] static constexpr auto kStaticColor = Color{Color::Red};

  // NEGATIVE
  [[maybe_unused]] static constexpr Axis kStaticAxis{Horizontal};

  // NEGATIVE
  [[maybe_unused]] thread_local Color threadColor{Color::Red};

#define FIXTURE_RED (Color{Color::Red})
  // NEGATIVE
  [[maybe_unused]] auto macroColor = FIXTURE_RED;
  // NEGATIVE
  [[maybe_unused]] auto nestedMacro = Color{FIXTURE_RED};
#undef FIXTURE_RED

#define FIXTURE_VALUE(value) value
  // NEGATIVE
  [[maybe_unused]] Color fromArgument(FIXTURE_VALUE(Color::Red));
  // NEGATIVE
  [[maybe_unused]] auto deducedArgument(FIXTURE_VALUE(Color::Red));
#undef FIXTURE_VALUE

#define FIXTURE_OPEN_PAREN (
#define FIXTURE_CLOSE_PAREN )
  // NEGATIVE
  [[maybe_unused]] Color macroParens FIXTURE_OPEN_PAREN Color::Red FIXTURE_CLOSE_PAREN;
  // NEGATIVE
  [[maybe_unused]] auto macroOpen = Color FIXTURE_OPEN_PAREN Color::Red);
  // NEGATIVE
  [[maybe_unused]] auto macroClose = Color(Color::Red FIXTURE_CLOSE_PAREN;
  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] auto groupedOperand = Color{FIXTURE_OPEN_PAREN Color::Red FIXTURE_CLOSE_PAREN};
  // POSITIVE | redundant initialization from a same-type enumerator
  [[maybe_unused]] auto groupedCast = FIXTURE_OPEN_PAREN Color{Color::Red} FIXTURE_CLOSE_PAREN;
#undef FIXTURE_CLOSE_PAREN
#undef FIXTURE_OPEN_PAREN

// The braces in each declaration balance only after preprocessing.
// clang-format off
#define FIXTURE_OPEN_BRACE {
#define FIXTURE_CLOSE_BRACE }
  // NEGATIVE
  [[maybe_unused]] auto macroOpenBrace = Color FIXTURE_OPEN_BRACE Color::Red};
  // NEGATIVE
  [[maybe_unused]] auto macroCloseBrace = Color{Color::Red FIXTURE_CLOSE_BRACE;
#undef FIXTURE_CLOSE_BRACE
#undef FIXTURE_OPEN_BRACE
  // clang-format on
}

namespace
{
  // Namespace-scope, static member, and static local primitives follow the
  // same literal and whole-initializer cast rules as ordinary locals.
  constexpr std::int32_t fixtureSum(std::int32_t lhs, std::int32_t rhs)
  {
    return lhs + rhs;
  }

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceCount = std::size_t{3};

  // POSITIVE
  [[maybe_unused]] inline constexpr auto kInlineVersion = std::uint32_t(1);

  // POSITIVE
  [[maybe_unused]] constexpr std::int32_t kBracedNamespacePrimitive{5};

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceInteger = 42;

  // POSITIVE
  [[maybe_unused]] constexpr auto kLiteralRatio = 0.5;

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceBool = true;

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceChar = 'a';

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceUnary = -(-5);

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceParen = ((5));

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceListInteger{42};

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceListBool{true};

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceListFloat{0.5f};

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceListChar{'a'};

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceListFunctionalCast{std::uint16_t(3)};

  // NEGATIVE
  [[maybe_unused]] constexpr auto kNamespaceListStaticCast{static_cast<std::uint16_t>(3)};

  // NEGATIVE
  [[maybe_unused]] constexpr auto kNamespaceListFunctionResult{fixtureSum(2, 3)};

  // NEGATIVE
  [[maybe_unused]] constexpr auto kNamespaceListArithmetic{fixtureSum(2, 3) * 2};

  // NEGATIVE
  [[maybe_unused]] constexpr std::size_t kExplicitCount = 3;

  // NEGATIVE
  [[maybe_unused]] constexpr auto kCastProduct = std::size_t{8} * 1024;

  // NEGATIVE
  [[maybe_unused]] constexpr auto kStaticCast = (static_cast<std::uint8_t>(255));

  // NEGATIVE
  [[maybe_unused]] char const* const kName = "fixture";

  // NEGATIVE
  [[maybe_unused]] std::int32_t* uninitializedPointer;

  // NEGATIVE
  [[maybe_unused]] LocalFoo namespaceObject{1};

  struct StaticMembers final
  {
    // POSITIVE
    static constexpr auto kMemberLimit = std::uint16_t{99};

    // POSITIVE
    static constexpr auto kMemberLiteral = 99;

    // POSITIVE
    static constexpr auto kMemberFloat = 0.5f;

    // POSITIVE
    static constexpr auto kMemberBool = false;

    // POSITIVE
    static constexpr auto kMemberChar = 'a';

    // POSITIVE
    static constexpr auto kMemberUnary = -(5);

    // POSITIVE
    static constexpr auto kMemberParen = ((true));

    // NEGATIVE
    static constexpr auto kMemberProduct = std::size_t{8} * 1024;

    // POSITIVE
    static constexpr auto kMemberListInteger{99};

    // POSITIVE
    static constexpr auto kMemberListBool{false};

    // POSITIVE
    static constexpr auto kMemberListFloat{0.5f};

    // POSITIVE
    static constexpr auto kMemberListChar{'a'};

    // POSITIVE
    inline static constexpr auto kInlineListLiteral{77};

    // POSITIVE
    inline static constexpr auto kInlineListFunctionalCast{std::uint16_t(4)};

    // NEGATIVE
    inline static constexpr auto kInlineListStaticCast{static_cast<std::uint16_t>(4)};

    // NEGATIVE
    static constexpr auto kMemberStaticCast = static_cast<std::int32_t>(1);

    // NEGATIVE
    inline static LocalFoo object{1};
  };

  // An instantiation reports the member once, at its template declaration.
  template<typename T>
  struct TemplateStatics final
  {
    // POSITIVE
    static constexpr auto kTemplateCount = std::size_t{2};

    // NEGATIVE
    static constexpr auto kDependentSize = sizeof(T);
  };

  // NEGATIVE
  [[maybe_unused]] constexpr auto kTemplateUse = TemplateStatics<std::int32_t>::kTemplateCount;
} // namespace

void testStaticLocal()
{
  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticLocal = std::int64_t{-1};

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticInteger = 1;

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticFloat = 0.5;

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticBool = false;

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticChar = 'a';

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticUnary = +('a');

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticParen = ((5));

  // POSITIVE
  [[maybe_unused]] static constexpr std::int32_t kStaticBraced{1};

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticListInteger{1};

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticListBool{false};

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticListFloat{0.5f};

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticListChar{'a'};

  // POSITIVE
  [[maybe_unused]] static constexpr auto kStaticListFunctionalCast{std::int32_t(2)};

  // NEGATIVE
  [[maybe_unused]] static constexpr auto kStaticListStaticCast{static_cast<std::int32_t>(2)};

  // NEGATIVE
  [[maybe_unused]] static constexpr std::size_t kStaticExplicit = 1;

  // NEGATIVE
  [[maybe_unused]] static constexpr auto kStaticProduct = std::size_t{8} * 1024;

  // NEGATIVE
  [[maybe_unused]] static constexpr auto kStaticCast = static_cast<std::int32_t>(1);

  // NEGATIVE
  [[maybe_unused]] static LocalFoo object{1};
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
