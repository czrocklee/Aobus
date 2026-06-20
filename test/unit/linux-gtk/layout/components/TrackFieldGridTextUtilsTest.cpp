// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/track/TrackFieldGridTextUtils.h"

#include "app/linux-gtk/track/TrackFieldUi.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <bits/basic_string.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

using namespace std::string_literals;

namespace ao::gtk::layout::track_field_grid::test
{
  using namespace ao::rt;

  namespace
  {
    rt::TrackDetailSnapshot makeSnap()
    {
      return rt::TrackDetailSnapshot{};
    }
  } // namespace

  TEST_CASE("validUtf8Text returns empty for empty input", "[gtk][unit][trackfieldgridtextutils]")
  {
    CHECK(validUtf8Text("").empty());
  }

  TEST_CASE("validUtf8Text passes valid ASCII through unchanged", "[gtk][unit][trackfieldgridtextutils]")
  {
    CHECK(validUtf8Text("hello world") == "hello world");
  }

  TEST_CASE("validUtf8Text preserves 2-byte UTF-8 sequences", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto const input = "caf\xC3\xA9"s; // "café"
    CHECK(validUtf8Text(input) == input);
  }

  TEST_CASE("validUtf8Text preserves 3-byte UTF-8 sequences", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto const input = "\xE4\xB8\xAD\xE6\x96\x87"s; // "中文"
    CHECK(validUtf8Text(input) == input);
  }

  TEST_CASE("validUtf8Text preserves 4-byte UTF-8 sequences", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto const input = "\xF0\x9D\x95\x8F"s; // U+1D54F "𝕏"
    CHECK(validUtf8Text(input) == input);
  }

  TEST_CASE("validUtf8Text replaces invalid bytes with substitution", "[gtk][unit][trackfieldgridtextutils]")
  {
    std::string const input = std::string{"before"} + '\xFF' + '\xFE' + "after";

    auto const result = validUtf8Text(input);

    CHECK(result.starts_with("before"));
    CHECK(result.ends_with("after"));
    CHECK(result.size() > std::string{"before"}.size() + std::string{"after"}.size());
  }

  TEST_CASE("validUtf8Text replaces lone continuation byte", "[gtk][unit][trackfieldgridtextutils]")
  {
    std::string const input = std::string{"a"} + '\x80' + "b";

    auto const result = validUtf8Text(input);

    CHECK(result.starts_with("a"));
    CHECK(result.ends_with("b"));
  }

  TEST_CASE("displayTextForField returns mixedText when aggregate is mixed", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Title).mixed = true;

    auto const result = displayTextForField(TrackField::Title, snap, "<<<mixed>>>", true);

    CHECK(result == "<<<mixed>>>");
  }

  TEST_CASE("displayTextForField returns 'Unknown' for unset Technical field when requested",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    auto const& def = *rt::trackFieldDefinition(TrackField::Codec);
    REQUIRE(def.category == TrackFieldCategory::Technical);

    auto const result = displayTextForField(TrackField::Codec, snap, kMultipleValuesText, true);

    CHECK(result == "Unknown");
  }

  TEST_CASE("displayTextForField returns empty for unset Technical field when not requested",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();

    auto const result = displayTextForField(TrackField::Codec, snap, kMultipleValuesText, false);

    CHECK(result.empty());
  }

  TEST_CASE("displayTextForField returns empty for unset non-Technical field", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();

    auto const result = displayTextForField(TrackField::Title, snap, kMultipleValuesText, true);

    CHECK(result.empty());
  }

  TEST_CASE("displayTextForField formats a populated text field", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Title).optValue = std::string{"Hello"};

    auto const result = displayTextForField(TrackField::Title, snap, kMultipleValuesText, true);

    CHECK(result == "Hello");
  }

  TEST_CASE("displayTextForField formats a populated numeric field", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Year).optValue = std::uint16_t{2024};

    auto const result = displayTextForField(TrackField::Year, snap, kMultipleValuesText, true);

    CHECK(result == "2024");
  }

  TEST_CASE("displayTextForField returns empty for field with no formatter", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    auto const* uiDef = trackFieldUiDefinition(TrackField::Quality);
    REQUIRE(uiDef != nullptr);
    REQUIRE(uiDef->formatValue == nullptr);

    rt::trackFieldArrayAt(snap.fields, TrackField::Quality).optValue = std::string{"anything"};

    auto const result = displayTextForField(TrackField::Quality, snap, kMultipleValuesText, true);

    CHECK(result.empty());
  }

  TEST_CASE("isProtectedFieldEditValue always protects the multi-value sentinel",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();

    CHECK(isProtectedFieldEditValue(TrackField::Title, snap, kMultipleValuesText, false));
  }

  TEST_CASE("isProtectedFieldEditValue protects the multi-value sentinel even when not mixed",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Title).optValue = std::string{"single"};

    CHECK(isProtectedFieldEditValue(TrackField::Title, snap, kMultipleValuesText, false));
  }

  TEST_CASE("isProtectedFieldEditValue protects composite mixed when protection is enabled",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Title).mixed = true;

    CHECK(isProtectedFieldEditValue(TrackField::Title, snap, kCompositeMixedText, true));
  }

  TEST_CASE("isProtectedFieldEditValue does not protect composite mixed when protection is disabled",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Title).mixed = true;

    CHECK_FALSE(isProtectedFieldEditValue(TrackField::Title, snap, kCompositeMixedText, false));
  }

  TEST_CASE("isProtectedFieldEditValue returns false for non-sentinel text on mixed aggregate",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Title).mixed = true;

    CHECK_FALSE(isProtectedFieldEditValue(TrackField::Title, snap, "anything else", true));
  }

  TEST_CASE("isProtectedFieldEditValue returns false for composite mixed when aggregate is not mixed",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Title).optValue = std::string{"single"};

    CHECK_FALSE(isProtectedFieldEditValue(TrackField::Title, snap, kCompositeMixedText, true));
  }

  TEST_CASE("isProtectedFieldEditValue returns false for ordinary text on a single-value field",
            "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();
    rt::trackFieldArrayAt(snap.fields, TrackField::Title).optValue = std::string{"single"};

    CHECK_FALSE(isProtectedFieldEditValue(TrackField::Title, snap, "edit", true));
  }

  TEST_CASE("isProtectedFieldEditValue returns false for empty edit value", "[gtk][unit][trackfieldgridtextutils]")
  {
    auto snap = makeSnap();

    CHECK_FALSE(isProtectedFieldEditValue(TrackField::Title, snap, "", true));
  }
} // namespace ao::gtk::layout::track_field_grid::test
