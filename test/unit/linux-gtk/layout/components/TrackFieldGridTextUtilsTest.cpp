// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/track/TrackFieldGridTextUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace ao::gtk::layout::track_field_grid::test
{
  TEST_CASE("validUtf8Text returns empty for empty input", "[gtk][unit][layout][track-field-grid-text]")
  {
    CHECK(validUtf8Text("").empty());
  }

  TEST_CASE("validUtf8Text passes valid ASCII through unchanged", "[gtk][unit][layout][track-field-grid-text]")
  {
    CHECK(validUtf8Text("hello world") == "hello world");
  }

  TEST_CASE("validUtf8Text preserves 2-byte UTF-8 sequences", "[gtk][unit][layout][track-field-grid-text]")
  {
    auto const input = std::string{"caf\xC3\xA9"}; // "café"
    CHECK(validUtf8Text(input) == input);
  }

  TEST_CASE("validUtf8Text preserves 3-byte UTF-8 sequences", "[gtk][unit][layout][track-field-grid-text]")
  {
    auto const input = std::string{"\xE4\xB8\xAD\xE6\x96\x87"}; // "中文"
    CHECK(validUtf8Text(input) == input);
  }

  TEST_CASE("validUtf8Text preserves 4-byte UTF-8 sequences", "[gtk][unit][layout][track-field-grid-text]")
  {
    auto const input = std::string{"\xF0\x9D\x95\x8F"}; // U+1D54F "𝕏"
    CHECK(validUtf8Text(input) == input);
  }

  TEST_CASE("validUtf8Text replaces invalid bytes with substitution", "[gtk][unit][layout][track-field-grid-text]")
  {
    std::string const input = std::string{"before"} + '\xFF' + '\xFE' + "after";

    auto const result = validUtf8Text(input);

    CHECK(result.starts_with("before"));
    CHECK(result.ends_with("after"));
    CHECK(result.size() > std::string_view{"before"}.size() + std::string_view{"after"}.size());
  }

  TEST_CASE("validUtf8Text replaces lone continuation byte", "[gtk][unit][layout][track-field-grid-text]")
  {
    std::string const input = std::string{"a"} + '\x80' + "b";

    auto const result = validUtf8Text(input);

    CHECK(result.starts_with("a"));
    CHECK(result.ends_with("b"));
  }
} // namespace ao::gtk::layout::track_field_grid::test
