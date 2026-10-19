// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>
#include <type_traits>

// These valid sources require advice only: exported diagnostics must contain
// no replacements, including for the ordinary local literal initializer.
// NO-FIX
void testCvAliasDeductions(std::int32_t input)
{
  using ConstInteger = std::int32_t const;
  using VolatileInteger = std::int32_t volatile;
  using ConstVolatileInteger = std::int32_t const volatile;

  // Direct-list bindings share the InitListExpr wrapper and must also export
  // diagnostics without replacement records.
  // POSITIVE
  auto directListLiteral{3};

  // POSITIVE
  auto directListCast{std::int32_t(4)};

  // POSITIVE
  auto literal = 1;

  // POSITIVE
  auto constAliasLiteral = ConstInteger{1};

  // POSITIVE
  auto constAliasCast = ConstInteger(input);

  // POSITIVE
  auto volatileAliasLiteral = VolatileInteger{2};

  // POSITIVE
  auto cvAliasCast = ConstVolatileInteger(input);

  static_assert(std::is_same_v<decltype(constAliasLiteral), std::int32_t>);
  static_assert(std::is_same_v<decltype(constAliasCast), std::int32_t>);
  static_assert(std::is_same_v<decltype(volatileAliasLiteral), std::int32_t>);
  static_assert(std::is_same_v<decltype(cvAliasCast), std::int32_t>);

  // Auto discarded the aliases' cv-qualifiers; advice must not imply otherwise.
  ++directListLiteral;
  ++directListCast;
  ++literal;
  ++constAliasLiteral;
  ++constAliasCast;
  ++volatileAliasLiteral;
  ++cvAliasCast;
}

namespace
{
  struct ExplicitInteger final
  {
    explicit constexpr operator std::int32_t() const { return 5; }
  };

  using ConstInteger = std::int32_t const;

  // POSITIVE
  auto namespaceCounter = ConstInteger{1};

  // POSITIVE
  constexpr auto kNamespaceConversion = std::int32_t{ExplicitInteger{}};

  struct StaticPrimitiveConversions final
  {
    // POSITIVE
    inline static auto memberCounter = ConstInteger{1};

    // POSITIVE
    static constexpr auto kConversion = std::int32_t{ExplicitInteger{}};
  };

  static_assert(std::is_same_v<decltype(namespaceCounter), std::int32_t>);
  static_assert(std::is_same_v<decltype(StaticPrimitiveConversions::memberCounter), std::int32_t>);
} // namespace

void testExplicitPrimitiveConversions()
{
  // NEGATIVE
  auto source = ExplicitInteger{};

  // Assignment from source would not call its explicit conversion operator.
  // POSITIVE
  [[maybe_unused]] auto bracedConversion = std::int32_t{source};

  // POSITIVE
  [[maybe_unused]] auto parenthesizedConversion = std::int32_t(source);

  // NEGATIVE
  [[maybe_unused]] auto staticConversion = static_cast<std::int32_t>(source);
}

void testStaticCvAliasAndExplicitConversions()
{
  ++namespaceCounter;
  ++StaticPrimitiveConversions::memberCounter;

  // POSITIVE
  static auto staticCounter = ConstInteger{1};

  // POSITIVE
  static constexpr auto kStaticConversion = std::int32_t{ExplicitInteger{}};

  static_assert(std::is_same_v<decltype(staticCounter), std::int32_t>);
  static_assert(kNamespaceConversion == 5);
  static_assert(StaticPrimitiveConversions::kConversion == 5);
  static_assert(kStaticConversion == 5);
  ++staticCounter;
}
