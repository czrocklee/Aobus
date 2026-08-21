// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/i18n/IcuTextOrdering.h>

#include "app/i18n/IcuTextOrderingDetail.h"
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/utility/UnicodeText.h>

#include <catch2/catch_test_macros.hpp>
#include <unicode/utypes.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::i18n::test
{
  namespace
  {
    std::unique_ptr<rt::TextOrderingPolicy> policyFor(std::string_view const locale)
    {
      auto policyRes = createIcuTextOrderingPolicy(locale);
      REQUIRE(policyRes);
      return std::move(*policyRes);
    }

    std::string keyFor(rt::TextOrderingPolicy const& policy, std::string_view const text)
    {
      auto key = std::string{};
      auto keyRes = policy.makeSortKeyInto(key, text);
      REQUIRE(keyRes);
      return key;
    }

    std::string caselessKeyFor(std::string_view const text)
    {
      auto keyRes = utility::makeUtf8CaselessKey(text);
      REQUIRE(keyRes);
      return std::move(*keyRes);
    }

    std::string hexBytes(std::string_view const value)
    {
      constexpr auto kHex = std::string_view{"0123456789abcdef"};
      auto result = std::string{};
      result.reserve(value.size() * 2);

      for (auto const ch : value)
      {
        auto const byte = static_cast<unsigned char>(ch);
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0f]);
      }

      return result;
    }
  } // namespace

  TEST_CASE("IcuTextOrdering - locale changes intentional alphabetic order", "[runtime][unit][collation]")
  {
    auto const germanPtr = policyFor("de-DE");
    auto const swedishPtr = policyFor("sv-SE");

    CHECK(keyFor(*germanPtr, "ä") < keyFor(*germanPtr, "z"));
    CHECK(keyFor(*swedishPtr, "z") < keyFor(*swedishPtr, "ä"));
  }

  TEST_CASE("IcuTextOrdering - explicit default folding owns case equivalence", "[runtime][unit][collation]")
  {
    auto const policyPtr = policyFor("de");

    CHECK(keyFor(*policyPtr, "Straße") == keyFor(*policyPtr, "STRASSE"));
    CHECK(keyFor(*policyPtr, "Σ") == keyFor(*policyPtr, "ς"));
  }

  TEST_CASE("IcuTextOrdering - punctuation and numeric text remain non-ignorable", "[runtime][unit][collation]")
  {
    auto const policyPtr = policyFor("en-US");

    CHECK(keyFor(*policyPtr, "").empty());
    CHECK_FALSE(keyFor(*policyPtr, "!!!").empty());
    CHECK_FALSE(keyFor(*policyPtr, "\u200D").empty());
    CHECK_FALSE(keyFor(*policyPtr, "\u00AD").empty());
    CHECK(keyFor(*policyPtr, "a-b") != keyFor(*policyPtr, "ab"));
    CHECK(keyFor(*policyPtr, "10") < keyFor(*policyPtr, "2"));
  }

  TEST_CASE("IcuTextOrdering - default folding retains its Turkic tradeoff", "[runtime][unit][collation]")
  {
    auto const policyPtr = policyFor("tr");

    CHECK(keyFor(*policyPtr, "I") == keyFor(*policyPtr, "i"));
    CHECK(keyFor(*policyPtr, "I") != keyFor(*policyPtr, "\u0131"));
  }

  TEST_CASE("IcuTextOrdering - secondary strength retains intentional tertiary ties", "[runtime][unit][collation]")
  {
    auto const englishPtr = policyFor("en-US");
    auto const japanesePtr = policyFor("ja-JP");

    REQUIRE(caselessKeyFor("ABC") != caselessKeyFor("ＡＢＣ"));
    REQUIRE(caselessKeyFor("メタル") != caselessKeyFor("ﾒﾀﾙ"));
    REQUIRE(caselessKeyFor("めたる") != caselessKeyFor("メタル"));
    CHECK(keyFor(*englishPtr, "ABC") == keyFor(*englishPtr, "ＡＢＣ"));
    CHECK(keyFor(*japanesePtr, "メタル") == keyFor(*japanesePtr, "ﾒﾀﾙ"));
    CHECK(keyFor(*japanesePtr, "めたる") == keyFor(*japanesePtr, "メタル"));
  }

  TEST_CASE("IcuTextOrdering - binary keys exclude ICU's terminal zero", "[runtime][unit][collation]")
  {
    auto const policyPtr = policyFor("de-DE");
    auto const key = keyFor(*policyPtr, "Dvořák");

    REQUIRE_FALSE(key.empty());
    CHECK_FALSE(key.contains('\0'));
    CHECK(key == keyFor(*policyPtr, "Dvořák"));
  }

  TEST_CASE("IcuTextOrdering - long text produces complete repeatable binary keys", "[runtime][unit][collation]")
  {
    auto const policyPtr = policyFor("en-US");
    auto const text = std::string(512, 'a');
    auto const key = keyFor(*policyPtr, text);

    REQUIRE(key.size() > 128);
    CHECK_FALSE(key.contains('\0'));
    CHECK(key == keyFor(*policyPtr, text));
    CHECK(key < keyFor(*policyPtr, text + "b"));
  }

  TEST_CASE("IcuTextOrdering - caller storage is replaced across successive keys", "[runtime][unit][collation]")
  {
    auto const policyPtr = policyFor("en-US");
    auto output = std::string{};

    REQUIRE(policyPtr->makeSortKeyInto(output, std::string(512, 'x')));
    REQUIRE(output.size() > 128);
    REQUIRE(policyPtr->makeSortKeyInto(output, "Dvořák"));
    auto const first = output;
    CHECK_FALSE(first.empty());

    REQUIRE(policyPtr->makeSortKeyInto(output, ""));
    CHECK(output.empty());

    REQUIRE(policyPtr->makeSortKeyInto(output, "Dvořák"));
    CHECK(output == first);
  }

  TEST_CASE("IcuTextOrdering - governed ICU data fixes candidate key bytes", "[runtime][unit][collation]")
  {
    auto const policyPtr = policyFor("de-DE");
    constexpr auto kFixtures = std::to_array<std::pair<std::string_view, std::string_view>>({
      {"Dvořák", "3155474d2b3f014290458805"},
      {"Ärzte", "2b4d5d513301459608"},
      {"Straße", "4f514d2b4f4f33010b"},
      {"STRASSE", "4f514d2b4f4f33010b"},
    });

    for (auto const& [text, expected] : kFixtures)
    {
      CHECK(hexBytes(keyFor(*policyPtr, text)) == expected);
    }
  }

  TEST_CASE("IcuTextOrdering - malformed locale and text are recoverable", "[runtime][unit][collation]")
  {
    auto invalidLocaleRes = createIcuTextOrderingPolicy("en_US!");
    REQUIRE_FALSE(invalidLocaleRes);
    CHECK(invalidLocaleRes.error().code == Error::Code::InvalidInput);

    auto const policyPtr = policyFor("en-US");
    auto key = std::string{};
    auto invalidTextRes = policyPtr->makeSortKeyInto(key, "bad\xFFtext");
    REQUIRE_FALSE(invalidTextRes);
    CHECK(invalidTextRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("IcuTextOrdering - ICU warnings retain the caller error class", "[runtime][unit][collation]")
  {
    CHECK(detail::collationErrorCode(U_USING_FALLBACK_WARNING, Error::Code::InvalidInput) == Error::Code::InvalidInput);
  }

  TEST_CASE("IcuTextOrdering - ICU construction failures keep typed error classes", "[runtime][unit][collation]")
  {
    CHECK(detail::collationErrorCode(U_MEMORY_ALLOCATION_ERROR) == Error::Code::ResourceExhausted);
    CHECK(detail::collationErrorCode(U_BUFFER_OVERFLOW_ERROR) == Error::Code::ValueTooLarge);
    CHECK(detail::collationErrorCode(U_ILLEGAL_ARGUMENT_ERROR) == Error::Code::InvalidInput);
    CHECK(detail::collationErrorCode(U_MISSING_RESOURCE_ERROR) == Error::Code::NotFound);
    CHECK(detail::collationErrorCode(U_FILE_ACCESS_ERROR) == Error::Code::NotFound);
    CHECK(detail::collationErrorCode(U_INVALID_FORMAT_ERROR) == Error::Code::CorruptData);
    CHECK(detail::collationErrorCode(U_INTERNAL_PROGRAM_ERROR) == Error::Code::InitFailed);
  }
} // namespace ao::i18n::test
