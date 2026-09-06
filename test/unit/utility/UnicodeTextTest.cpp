// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/UnicodeText.h>

#include <ao/Error.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <unicode/uchar.h>
#include <unicode/uvernum.h>
#include <unicode/uversion.h>

#include <cstddef>
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

    // UVersionInfo is an ICU array typedef, so it has no 'auto x = Type{}' spelling
    // and ICU's own accessor takes it by decayed pointer.
    // NOLINTNEXTLINE(aobus-modernize-local-initialization-style)
    UVersionInfo unicodeVersion{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    ::u_getUnicodeVersion(unicodeVersion);
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

  TEST_CASE("UnicodeText - checks grapheme boundaries without accepting interior scalar or cluster offsets",
            "[utility][unit][unicode]")
  {
    for (auto const cluster :
         {std::string_view{"a\u0301"}, std::string_view{"🇯🇵"}, std::string_view{"👩🏽‍💻"}})
    {
      auto const text = std::string{"x"} + std::string{cluster} + "y";

      for (std::size_t offset = 0; offset <= text.size(); ++offset)
      {
        CAPTURE(text, offset);
        auto const boundaryRes = isUtf8GraphemeBoundary(text, offset);
        REQUIRE(boundaryRes);
        CHECK(*boundaryRes == (offset == 0 || offset == 1 || offset == text.size() - 1 || offset == text.size()));
      }
    }

    auto const emptyRes = isUtf8GraphemeBoundary({}, 0);
    REQUIRE(emptyRes);
    CHECK(*emptyRes);
    auto const outsideRes = isUtf8GraphemeBoundary("x", 2);
    REQUIRE_FALSE(outsideRes);
    CHECK(outsideRes.error().code == Error::Code::InvalidInput);

    auto const invalid = bytes({0x80});

    for (auto const offset : {std::size_t{0}, invalid.size()})
    {
      auto const invalidRes = isUtf8GraphemeBoundary(invalid, offset);
      REQUIRE_FALSE(invalidRes);
      CHECK(invalidRes.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("UnicodeText - finds the previous extended grapheme boundary in UTF-8 bytes", "[utility][unit][unicode]")
  {
    auto const checkBoundary = [](std::string_view const text, std::size_t const expected)
    {
      auto const boundaryRes = previousUtf8GraphemeBoundary(text, text.size());
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

    auto const invalidRes = previousUtf8GraphemeBoundary(bytes({0x80}), 1);
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("UnicodeText - walks extended grapheme boundaries from an interior offset", "[utility][unit][unicode]")
  {
    auto const family = std::string_view{"👨‍👩‍👧‍👦"};
    auto const text = std::string{"a"} + std::string{family} + "b";

    SECTION("A cursor walks whole clusters in both directions")
    {
      auto const afterLetterRes = nextUtf8GraphemeBoundary(text, 0);
      REQUIRE(afterLetterRes);
      CHECK(*afterLetterRes == 1);

      auto const afterFamilyRes = nextUtf8GraphemeBoundary(text, *afterLetterRes);
      REQUIRE(afterFamilyRes);
      CHECK(*afterFamilyRes == 1 + family.size());

      auto const beforeFamilyRes = previousUtf8GraphemeBoundary(text, *afterFamilyRes);
      REQUIRE(beforeFamilyRes);
      CHECK(*beforeFamilyRes == 1);
    }

    SECTION("Each end reports itself rather than running off the text")
    {
      auto const atStartRes = previousUtf8GraphemeBoundary(text, 0);
      REQUIRE(atStartRes);
      CHECK(*atStartRes == 0);

      auto const atEndRes = nextUtf8GraphemeBoundary(text, text.size());
      REQUIRE(atEndRes);
      CHECK(*atEndRes == text.size());
    }

    SECTION("An offset inside a cluster resolves to that cluster's own edges")
    {
      auto const insideFamily = 1 + std::string_view{"👨"}.size();

      auto const clusterStartRes = previousUtf8GraphemeBoundary(text, insideFamily);
      REQUIRE(clusterStartRes);
      CHECK(*clusterStartRes == 1);

      auto const clusterEndRes = nextUtf8GraphemeBoundary(text, insideFamily);
      REQUIRE(clusterEndRes);
      CHECK(*clusterEndRes == 1 + family.size());
    }

    SECTION("An offset inside a multibyte scalar resolves to the same cluster edges")
    {
      // The cluster's first scalar is four bytes wide, so an offset inside it
      // rounds down onto the cluster's own start; a backward search that then
      // looked strictly earlier would leave the cluster entirely.
      std::size_t const insideFirstScalar = 2;

      auto const clusterStartRes = previousUtf8GraphemeBoundary(text, insideFirstScalar);
      REQUIRE(clusterStartRes);
      CHECK(*clusterStartRes == 1);

      auto const clusterEndRes = nextUtf8GraphemeBoundary(text, insideFirstScalar);
      REQUIRE(clusterEndRes);
      CHECK(*clusterEndRes == 1 + family.size());

      // A two-byte scalar standing alone as its own cluster answers the same way.
      auto const accented = std::string{"a\u00e9b"};

      auto const accentStartRes = previousUtf8GraphemeBoundary(accented, std::size_t{2});
      REQUIRE(accentStartRes);
      CHECK(*accentStartRes == 1);

      auto const accentEndRes = nextUtf8GraphemeBoundary(accented, std::size_t{2});
      REQUIRE(accentEndRes);
      CHECK(*accentEndRes == 3);
    }

    SECTION("An offset past the end is rejected instead of clamped")
    {
      auto const previousRes = previousUtf8GraphemeBoundary(text, text.size() + 1);
      REQUIRE_FALSE(previousRes);
      CHECK(previousRes.error().code == Error::Code::InvalidInput);

      auto const nextRes = nextUtf8GraphemeBoundary(text, text.size() + 1);
      REQUIRE_FALSE(nextRes);
      CHECK(nextRes.error().code == Error::Code::InvalidInput);
    }
  }
} // namespace ao::utility::test
