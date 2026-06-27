// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/track/TrackFieldUi.h"

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace ao::gtk::test
{
  using namespace ao::rt;

  namespace
  {
    using Raw = TrackFieldRawValue;
    using Edit = TrackFieldEditValue;

    auto const kAllFields = trackFieldDefinitions();
  } // namespace

  TEST_CASE("TrackFieldUi registry exposes one adapter definition per runtime field", "[gtk][unit][track-field-ui]")
  {
    auto const uiDefs = trackFieldUiDefinitions();
    CHECK(uiDefs.size() == ao::rt::kTrackFieldCount);

    for (auto const& rtDef : kAllFields)
    {
      INFO("Field " << rtDef.id << " must have a UI definition");
      CHECK(trackFieldUiDefinition(rtDef.field) != nullptr);
    }
  }

  TEST_CASE("TrackFieldUi registry exposes row text readers for presentable fields", "[gtk][unit][track-field-ui]")
  {
    for (auto const& rtDef : kAllFields)
    {
      if (!rtDef.presentable)
      {
        continue;
      }

      auto const* uiDef = trackFieldUiDefinition(rtDef.field);

      INFO("Field: " << rtDef.id);
      REQUIRE(uiDef != nullptr);
      CHECK(uiDef->readRowText != nullptr);
    }
  }

  TEST_CASE("TrackFieldUi registry wires inline-edit hooks for editable metadata fields", "[gtk][unit][track-field-ui]")
  {
    auto const editableFields = std::to_array({TrackField::Title,
                                               TrackField::Artist,
                                               TrackField::Album,
                                               TrackField::AlbumArtist,
                                               TrackField::Genre,
                                               TrackField::Composer,
                                               TrackField::Work,
                                               TrackField::Year,
                                               TrackField::DiscNumber,
                                               TrackField::DiscTotal,
                                               TrackField::TrackNumber,
                                               TrackField::TrackTotal});

    for (auto const field : editableFields)
    {
      auto const* def = trackFieldUiDefinition(field);

      INFO("Field: " << trackFieldId(field));
      REQUIRE(def != nullptr);
      CHECK(canInlineEdit(*def));
      CHECK(def->parseInlineEdit != nullptr);
      CHECK(def->writePatch != nullptr);
      CHECK(def->readRowEditValue != nullptr);
      CHECK(def->applyRowEditValue != nullptr);
    }

    CHECK_FALSE(canInlineEdit(*trackFieldUiDefinition(TrackField::Duration)));
    CHECK_FALSE(canInlineEdit(*trackFieldUiDefinition(TrackField::Tags)));
  }

  TEST_CASE("TrackFieldUi registry delegates representative raw field formatting", "[gtk][unit][track-field-ui]")
  {
    auto const* title = trackFieldUiDefinition(TrackField::Title);
    auto const* year = trackFieldUiDefinition(TrackField::Year);
    auto const* duration = trackFieldUiDefinition(TrackField::Duration);
    auto const* bitrate = trackFieldUiDefinition(TrackField::Bitrate);

    REQUIRE(title != nullptr);
    REQUIRE(year != nullptr);
    REQUIRE(duration != nullptr);
    REQUIRE(bitrate != nullptr);
    REQUIRE(title->formatValue != nullptr);
    REQUIRE(year->formatValue != nullptr);
    REQUIRE(duration->formatValue != nullptr);
    REQUIRE(bitrate->formatValue != nullptr);

    CHECK(title->formatValue(Raw{std::in_place_type<std::string>, "Hello"}) == "Hello");
    CHECK(title->formatValue(Raw{std::monostate{}}).empty());
    CHECK(year->formatValue(Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(2024)}) == "2024");
    CHECK(year->formatValue(Raw{std::in_place_type<std::string>, "not a number"}).empty());
    CHECK(duration->formatValue(Raw{std::in_place_type<TrackFieldDuration>, TrackFieldDuration{225000}}) == "3:45");
    CHECK(bitrate->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(320000)}) ==
          "320 kbps");
  }

  TEST_CASE("TrackFieldUi registry maps inline edits into metadata patches", "[gtk][unit][track-field-ui]")
  {
    SECTION("text field writes matching optional string")
    {
      auto const* def = trackFieldUiDefinition(TrackField::Title);
      REQUIRE(def != nullptr);
      REQUIRE(def->writePatch != nullptr);

      auto patch = MetadataPatch{};
      auto const value = Edit{std::in_place_type<std::string>, "New Title"};
      def->writePatch(TrackFieldEditContext{.patch = patch, .value = value});

      REQUIRE(patch.optTitle.has_value());
      CHECK(*patch.optTitle == "New Title");
    }

    SECTION("numeric field writes matching optional integer")
    {
      auto const* def = trackFieldUiDefinition(TrackField::TrackNumber);
      REQUIRE(def != nullptr);
      REQUIRE(def->writePatch != nullptr);

      auto patch = MetadataPatch{};
      auto const value = Edit{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(7)};
      def->writePatch(TrackFieldEditContext{.patch = patch, .value = value});

      REQUIRE(patch.optTrackNumber.has_value());
      CHECK(*patch.optTrackNumber == 7);
    }

    SECTION("tag field is intentionally edited through the tag controller")
    {
      auto const* def = trackFieldUiDefinition(TrackField::Tags);

      REQUIRE(def != nullptr);
      CHECK(def->writePatch == nullptr);
    }
  }

  TEST_CASE("TrackFieldUi registry leaves future synthetic quality field read-only", "[gtk][unit][track-field-ui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Quality);

    REQUIRE(def != nullptr);
    CHECK(def->readRowText != nullptr);
    CHECK(def->formatValue == nullptr);
    CHECK_FALSE(canInlineEdit(*def));
  }
} // namespace ao::gtk::test
