// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/String.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace ao::utility::test
{
  TEST_CASE("String - ASCII character operations are locale independent", "[utility][unit][string]")
  {
    CHECK(isAsciiAlpha('A'));
    CHECK(isAsciiAlpha('z'));
    CHECK_FALSE(isAsciiAlpha('@'));
    CHECK_FALSE(isAsciiAlpha('['));

    CHECK(isAsciiDigit('0'));
    CHECK(isAsciiDigit('9'));
    CHECK_FALSE(isAsciiDigit('/'));
    CHECK_FALSE(isAsciiDigit(':'));

    CHECK(isAsciiAlphaNumeric('Q'));
    CHECK(isAsciiAlphaNumeric('7'));
    CHECK_FALSE(isAsciiAlphaNumeric('-'));

    CHECK(isAsciiWhitespace(' '));
    CHECK(isAsciiWhitespace('\t'));
    CHECK(isAsciiWhitespace('\n'));
    CHECK(isAsciiWhitespace('\r'));
    CHECK(isAsciiWhitespace('\f'));
    CHECK(isAsciiWhitespace('\v'));
    CHECK_FALSE(isAsciiWhitespace('\0'));

    CHECK(isAsciiPrintable(' '));
    CHECK(isAsciiPrintable('~'));
    CHECK_FALSE(isAsciiPrintable('\x1f'));
    CHECK_FALSE(isAsciiPrintable('\x7f'));

    CHECK(toAsciiLower('A') == 'a');
    CHECK(toAsciiLower('Z') == 'z');
    CHECK(toAsciiLower('a') == 'a');
    CHECK(toAsciiUpper('a') == 'A');
    CHECK(toAsciiUpper('z') == 'Z');
    CHECK(toAsciiUpper('A') == 'A');

    auto const utf8Byte = static_cast<char>(0xC3);
    CHECK_FALSE(isAsciiAlpha(utf8Byte));
    CHECK_FALSE(isAsciiWhitespace(utf8Byte));
    CHECK(toAsciiLower(utf8Byte) == utf8Byte);
    CHECK(toAsciiUpper(utf8Byte) == utf8Byte);
  }

  TEST_CASE("String - ASCII transforms preserve UTF-8 bytes", "[utility][unit][string][utf8]")
  {
    auto const text = std::string{" \tDVO\xC5\x98\xC3\x81K \n"};

    CHECK(toLower(text) == " \tdvo\xC5\x98\xC3\x81k \n");
    CHECK(trim(text) == std::string_view{text}.substr(2, text.size() - 4));

    auto const nonAsciiWhitespace = std::string{"\xC2\xA0text\xC2\xA0"};
    CHECK(trim(nonAsciiWhitespace) == nonAsciiWhitespace);
  }
} // namespace ao::utility::test
