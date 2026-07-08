// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/field/TrackFieldEditCodec.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <variant>

namespace ao::uimodel::test
{
  TEST_CASE("TrackFieldEditCodec - preserves text edit input", "[uimodel][unit][field][codec]")
  {
    auto const editValue = makeTextEditValue(" Test ");
    auto const* text = std::get_if<std::string>(&editValue);
    REQUIRE(text != nullptr);
    CHECK(*text == " Test ");

    auto const parsed = parseTextEditValue("New Title");
    REQUIRE(parsed.has_value());
    auto const* parsedText = std::get_if<std::string>(&*parsed);
    REQUIRE(parsedText != nullptr);
    CHECK(*parsedText == "New Title");
  }

  TEST_CASE("TrackFieldEditCodec - parses uint16 edit text", "[uimodel][unit][field][codec]")
  {
    SECTION("valid number")
    {
      auto const result = parseUint16EditValue("  42  ");
      REQUIRE(result.has_value());
      auto const* value = std::get_if<std::uint16_t>(&*result);
      REQUIRE(value != nullptr);
      CHECK(*value == 42);
    }

    SECTION("empty input clears to zero")
    {
      auto const result = parseUint16EditValue("    ");
      REQUIRE(result.has_value());
      auto const* value = std::get_if<std::uint16_t>(&*result);
      REQUIRE(value != nullptr);
      CHECK(*value == 0);
    }

    SECTION("invalid number returns a rejected format error")
    {
      auto const result = parseUint16EditValue("abc");

      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message == "Enter a whole number from 0 to 65535.");
    }

    SECTION("negative input is rejected")
    {
      CHECK_FALSE(parseUint16EditValue("-1").has_value());
    }

    SECTION("out-of-range input is rejected")
    {
      CHECK_FALSE(parseUint16EditValue("65536").has_value());
    }
  }
} // namespace ao::uimodel::test
