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
  [[maybe_unused]] auto listLiteral{263};

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

namespace
{
  // Namespace-scope, static member, and static local primitives follow the
  // same literal and whole-initializer cast rules as ordinary locals.
  constexpr std::int32_t fixtureSum(std::int32_t lhs, std::int32_t rhs)
  {
    return lhs + rhs;
  }

  // POSITIVE
  [[maybe_unused]] constexpr auto kNamespaceCount = std::size_t{263};

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
  [[maybe_unused]] constexpr std::size_t kExplicitCount = 263;

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
