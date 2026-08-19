// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/UnicodeText.h>

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>
#include <unicode/uchar.h>
#include <unicode/utypes.h>
#include <unicode/uversion.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace ao::utility::test
{
  namespace
  {
    std::string bytes(std::initializer_list<unsigned char> const values)
    {
      auto result = std::string{};
      result.reserve(values.size());
      for (auto const value : values)
      {
        result.push_back(static_cast<char>(value));
      }
      return result;
    }
  } // namespace

  TEST_CASE("UnicodeText - uses the governed ICU and Unicode data", "[utility][unit][unicode]")
  {
    CHECK(std::string_view{U_ICU_VERSION} == "78.3");

    UVersionInfo unicodeVersion{};
    u_getUnicodeVersion(unicodeVersion);
    CHECK(unicodeVersion[0] == 17);
    CHECK(unicodeVersion[1] == 0);
  }

  TEST_CASE("UnicodeText - validates Unicode scalar UTF-8", "[utility][unit][unicode]")
  {
    CHECK(validateUtf8(""));
    CHECK(validateUtf8("Dvořák"));
    CHECK(validateUtf8("誰か、海を。"));
    CHECK(validateUtf8("👩🏽‍💻"));

    for (auto const& invalid : {
           bytes({0x80}),                   // lone continuation byte
           bytes({0xE2, 0x82}),             // truncated sequence
           bytes({0xC0, 0xAF}),             // overlong encoding
           bytes({0xED, 0xA0, 0x80}),       // UTF-16 surrogate
           bytes({0xF4, 0x90, 0x80, 0x80}), // above U+10FFFF
         })
    {
      auto const result = validateUtf8(invalid);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("UnicodeText - normalizes valid text to NFC", "[utility][unit][unicode]")
  {
    auto const nullBackedEmpty = std::string_view{};
    auto const decomposed = std::string{"Dvor\u030Ca\u0301k"};
    auto const emptyCheckRes = isUtf8Nfc(nullBackedEmpty);
    auto const emptyNormalizedRes = normalizeUtf8Nfc(nullBackedEmpty);
    auto const decomposedCheckRes = isUtf8Nfc(decomposed);
    auto const composedCheckRes = isUtf8Nfc("Dvořák");
    auto const normalizedRes = normalizeUtf8Nfc(decomposed);

    REQUIRE(emptyCheckRes);
    CHECK(*emptyCheckRes);
    REQUIRE(emptyNormalizedRes);
    CHECK(emptyNormalizedRes->empty());
    REQUIRE(decomposedCheckRes);
    CHECK_FALSE(*decomposedCheckRes);
    REQUIRE(composedCheckRes);
    CHECK(*composedCheckRes);
    REQUIRE(normalizedRes);
    CHECK(*normalizedRes == "Dvořák");

    auto const idempotentRes = normalizeUtf8Nfc(*normalizedRes);
    REQUIRE(idempotentRes);
    CHECK(*idempotentRes == *normalizedRes);

    auto const invalidRes = normalizeUtf8Nfc(bytes({0xC0, 0xAF}));
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::InvalidInput);

    auto const invalidCheckRes = isUtf8Nfc(bytes({0xC0, 0xAF}));
    REQUIRE_FALSE(invalidCheckRes);
    CHECK(invalidCheckRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("UnicodeText - builds locale-independent NFC caseless keys", "[utility][unit][unicode]")
  {
    auto const emptyRes = makeUtf8CaselessKey(std::string_view{});
    auto const upperRes = makeUtf8CaselessKey("DVOŘÁK");
    auto const decomposedRes = makeUtf8CaselessKey("Dvor\u030Ca\u0301k");
    auto const sharpSRes = makeUtf8CaselessKey("Straße");
    auto const medialSigmaRes = makeUtf8CaselessKey("ΟΣ");
    auto const finalSigmaRes = makeUtf8CaselessKey("ος");

    REQUIRE(emptyRes);
    CHECK(emptyRes->empty());
    REQUIRE(upperRes);
    REQUIRE(decomposedRes);
    REQUIRE(sharpSRes);
    REQUIRE(medialSigmaRes);
    REQUIRE(finalSigmaRes);
    CHECK(*upperRes == "dvořák");
    CHECK(*decomposedRes == "dvořák");
    CHECK(*sharpSRes == "strasse");
    CHECK(*medialSigmaRes == *finalSigmaRes);
  }

  TEST_CASE("UnicodeText - finds the previous extended grapheme boundary in UTF-8 bytes", "[utility][unit][unicode]")
  {
    auto const checkBoundary = [](std::string_view const text, std::size_t const expected)
    {
      auto const boundaryRes = previousUtf8GraphemeBoundary(text);
      REQUIRE(boundaryRes);
      CHECK(*boundaryRes == expected);
    };

    checkBoundary("", 0);
    checkBoundary("a", 0);
    checkBoundary("ab", 1);
    checkBoundary("xa\u0301", 1);
    checkBoundary("x✈️", 1);
    checkBoundary("🇯🇵", 0);
    checkBoundary("🇯🇵🇺🇸", std::string_view{"🇯🇵"}.size());
    checkBoundary("x🇯🇵", 1);
    checkBoundary("x👨‍👩‍👧‍👦", 1);

    auto const invalidRes = previousUtf8GraphemeBoundary(bytes({0x80}));
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::InvalidInput);
  }
} // namespace ao::utility::test
