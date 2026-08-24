// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/FromChars.h>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>

namespace ao::utility::test
{
  namespace
  {
    template<typename T>
    std::from_chars_result parse(std::string_view text, T& value)
    {
      return fromChars(text.data(), text.data() + text.size(), value);
    }

    template<typename T>
    bool parsesFully(std::string_view text, T& value)
    {
      auto const [ptr, ec] = parse(text, value);
      return ec == std::errc{} && ptr == text.data() + text.size();
    }
  } // namespace

  TEST_CASE("FromChars - parses plain decimal values", "[utility][unit][from-chars]")
  {
    double value = 0.0;
    REQUIRE(parsesFully("3.5", value));
    CHECK(value == 3.5);

    REQUIRE(parsesFully("-2.25", value));
    CHECK(value == -2.25);

    REQUIRE(parsesFully("42", value));
    CHECK(value == 42.0);
  }

  TEST_CASE("FromChars - parses exponent notation", "[utility][unit][from-chars]")
  {
    double value = 0.0;
    REQUIRE(parsesFully("1.5e3", value));
    CHECK(value == 1500.0);
  }

  TEST_CASE("FromChars - rejects what std::from_chars rejects", "[utility][unit][from-chars]")
  {
    double value = 0.0;

    CHECK(parse(" 1.0", value).ec == std::errc::invalid_argument);
    CHECK(parse("+1.0", value).ec == std::errc::invalid_argument);
    CHECK(parse("", value).ec == std::errc::invalid_argument);
    CHECK(parse("abc", value).ec == std::errc::invalid_argument);
  }

  TEST_CASE("FromChars - stops before a hexadecimal marker like the general format", "[utility][unit][from-chars]")
  {
    double value = 1.0;
    auto const text = std::string_view{"0x10"};

    auto const [ptr, ec] = parse(text, value);

    CHECK(ec == std::errc{});
    CHECK(value == 0.0);
    CHECK(ptr == text.data() + 1);
  }

  TEST_CASE("FromChars - reports the unconsumed tail", "[utility][unit][from-chars]")
  {
    double value = 0.0;
    auto const text = std::string_view{"1.5abc"};

    auto const [ptr, ec] = parse(text, value);

    CHECK(ec == std::errc{});
    CHECK(value == 1.5);
    CHECK(ptr == text.data() + 3);
  }

  TEST_CASE("FromChars - accepts infinities and NaNs", "[utility][unit][from-chars]")
  {
    double value = 0.0;

    auto const infinity = std::string_view{"-INFINITY"};
    auto const infinityResult = parse(infinity, value);
    CHECK(infinityResult.ec == std::errc{});
    CHECK(value == -std::numeric_limits<double>::infinity());
    CHECK(infinityResult.ptr == infinity.data() + infinity.size());

    auto const truncated = std::string_view{"infrared"};
    auto const truncatedResult = parse(truncated, value);
    CHECK(truncatedResult.ec == std::errc{});
    CHECK(value == std::numeric_limits<double>::infinity());
    CHECK(truncatedResult.ptr == truncated.data() + 3);

    auto const notANumber = std::string_view{"nan(payload)"};
    auto const notANumberResult = parse(notANumber, value);
    CHECK(notANumberResult.ec == std::errc{});
    CHECK(std::isnan(value));
    CHECK(notANumberResult.ptr == notANumber.data() + notANumber.size());
  }

  TEST_CASE("FromChars - keeps an exponent only when digits follow it", "[utility][unit][from-chars]")
  {
    double value = 0.0;
    auto const text = std::string_view{"1e"};

    auto const [ptr, ec] = parse(text, value);

    CHECK(ec == std::errc{});
    CHECK(value == 1.0);
    CHECK(ptr == text.data() + 1);
  }

  TEST_CASE("FromChars - accepts a missing integer or fraction but not a lone point", "[utility][unit][from-chars]")
  {
    double value = 0.0;

    REQUIRE(parsesFully(".5", value));
    CHECK(value == 0.5);

    REQUIRE(parsesFully("1.", value));
    CHECK(value == 1.0);

    CHECK(parse(".", value).ec == std::errc::invalid_argument);
  }

  TEST_CASE("FromChars - is independent of the ambient locale", "[utility][unit][from-chars]")
  {
    double value = 0.0;
    auto const text = std::string_view{"3,14"};

    auto const [ptr, ec] = parse(text, value);

    CHECK(ec == std::errc{});
    CHECK(value == 3.0);
    CHECK(ptr == text.data() + 1);
  }

  TEST_CASE("FromChars - accepts representable subnormal values", "[utility][regression][from-chars]")
  {
    double value = 1.0;
    REQUIRE(parsesFully("5e-324", value));
    CHECK(value == std::numeric_limits<double>::denorm_min());

    float floatValue = 1.0F;
    REQUIRE(parsesFully("1e-45", floatValue));
    CHECK(floatValue == std::numeric_limits<float>::denorm_min());
  }

  TEST_CASE("FromChars - reports true range errors without changing the output", "[utility][regression][from-chars]")
  {
    double value = 42.0;
    auto const underflow = std::string_view{"1e-9999"};
    auto const underflowResult = parse(underflow, value);
    CHECK(underflowResult.ec == std::errc::result_out_of_range);
    CHECK(underflowResult.ptr == underflow.data() + underflow.size());
    CHECK(value == 42.0);

    auto const overflow = std::string_view{"1e9999"};
    auto const overflowResult = parse(overflow, value);
    CHECK(overflowResult.ec == std::errc::result_out_of_range);
    CHECK(overflowResult.ptr == overflow.data() + overflow.size());
    CHECK(value == 42.0);
  }

  TEST_CASE("FromChars - rounds finite values according to from_chars", "[utility][unit][from-chars]")
  {
    double value = 0.0;
    REQUIRE(parsesFully("1.7976931348623157e308", value));
    CHECK(value == std::numeric_limits<double>::max());

    REQUIRE(parsesFully("9007199254740993", value));
    CHECK(value == 9007199254740992.0);
  }

  TEST_CASE("FromChars - the entry point handles integers and floats", "[utility][unit][from-chars]")
  {
    std::uint32_t whole = 0;
    auto const digits = std::string_view{"1234"};
    auto const wholeResult = fromChars(digits.data(), digits.data() + digits.size(), whole);
    CHECK(wholeResult.ec == std::errc{});
    CHECK(whole == 1234U);

    float fraction = 0.0F;
    auto const text = std::string_view{"0.5"};
    auto const fractionResult = fromChars(text.data(), text.data() + text.size(), fraction);
    CHECK(fractionResult.ec == std::errc{});
    CHECK(fraction == 0.5F);
  }
} // namespace ao::utility::test
