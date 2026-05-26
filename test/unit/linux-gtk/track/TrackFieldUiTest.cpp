// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/track/TrackFieldUi.h"

#include <ao/Error.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::gtk::test
{
  using namespace ao::gtk;
  using namespace ao::rt;

  namespace
  {
    using Raw = detail::TrackFieldRawValue;
    using Edit = detail::TrackFieldEditValue;
    using Dur = detail::Duration;

    auto const kAllFields = trackFieldDefinitions();
  } // namespace

  TEST_CASE("TrackFieldUi registry has entry for every runtime field", "[gtk][unit][trackfieldui]")
  {
    for (auto const& rtDef : kAllFields)
    {
      INFO("Field " << rtDef.id << " must have a UI definition");
      CHECK(trackFieldUiDefinition(rtDef.field) != nullptr);
    }
  }

  TEST_CASE("TrackFieldUi registry has kTrackFieldCount definitions", "[gtk][unit][trackfieldui]")
  {
    auto const uiDefs = trackFieldUiDefinitions();

    CHECK(uiDefs.size() == ao::rt::kTrackFieldCount);
  }

  TEST_CASE("TrackFieldUi formatValue for text fields returns raw string", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Title);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<std::string>, "Hello"});

    CHECK(formatted == "Hello");
  }

  TEST_CASE("TrackFieldUi formatValue for text returns empty for non-string variant", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Title);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::monostate{}});

    CHECK(formatted.empty());
  }

  TEST_CASE("TrackFieldUi formatValue for uint16_t returns formatted number", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Year);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(2024)});

    CHECK(formatted == "2024");
  }

  TEST_CASE("TrackFieldUi formatValue for uint16_t returns empty for zero", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::TrackNumber);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(0)});

    CHECK(formatted.empty());
  }

  TEST_CASE("TrackFieldUi formatValue for Duration produces M:SS format", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Duration);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<Dur>, Dur{225000}});

    CHECK(formatted == "3:45");
  }

  TEST_CASE("TrackFieldUi formatValue for Duration with hours", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Duration);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<Dur>, Dur{3723000}});

    CHECK(formatted == "1:2:03");
  }

  TEST_CASE("TrackFieldUi formatValue for Duration returns empty for zero", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Duration);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<Dur>, Dur{0}});

    CHECK(formatted.empty());
  }

  TEST_CASE("TrackFieldUi formatValue for SampleRate appends Hz", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::SampleRate);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(44100)});

    CHECK(formatted == "44100 Hz");
  }

  TEST_CASE("TrackFieldUi formatValue for Bitrate appends kbps", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Bitrate);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(320000)});

    CHECK(formatted == "320 kbps");
  }

  TEST_CASE("TrackFieldUi raw values distinguish formatted collisions", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Bitrate);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const low = Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(1000)};
    auto const high = Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(1999)};

    REQUIRE(def->formatValue(low) == def->formatValue(high));
    CHECK(low != high);
  }

  TEST_CASE("TrackFieldUi formatValue for Channels returns Mono/Stereo", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Channels);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    CHECK(def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(1)}) == "Mono");
    CHECK(def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(2)}) == "Stereo");
  }

  TEST_CASE("TrackFieldUi formatValue for FileSize", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::FileSize);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const mb = def->formatValue(Raw{std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(5242880)});

    CHECK(mb == "5.0 MB");
  }

  TEST_CASE("TrackFieldUi formatValue for BitDepth", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::BitDepth);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(24)});

    CHECK(formatted == "24-bit");
  }

  TEST_CASE("TrackFieldUi formatValue returns empty for non-matching variant", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Year);

    REQUIRE(def != nullptr);
    REQUIRE(def->formatValue != nullptr);

    auto const formatted = def->formatValue(Raw{std::in_place_type<std::string>, "not a number"});

    CHECK(formatted.empty());
  }

  TEST_CASE("TrackFieldUi parseInlineEdit for text fields produces edit string", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Title);

    REQUIRE(def != nullptr);
    REQUIRE(def->parseInlineEdit != nullptr);

    auto const result = def->parseInlineEdit("New Title");

    REQUIRE(result);

    auto const* value = std::get_if<std::string>(&*result);

    REQUIRE(value != nullptr);
    CHECK(*value == "New Title");
  }

  TEST_CASE("TrackFieldUi parseInlineEdit for numeric fields produces edit uint16_t", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Year);

    REQUIRE(def != nullptr);
    REQUIRE(def->parseInlineEdit != nullptr);

    auto const result = def->parseInlineEdit(" 2024 ");

    REQUIRE(result);

    auto const* value = std::get_if<std::uint16_t>(&*result);

    REQUIRE(value != nullptr);
    CHECK(*value == 2024);
  }

  TEST_CASE("TrackFieldUi parseInlineEdit for empty numeric text clears value", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::TrackNumber);

    REQUIRE(def != nullptr);
    REQUIRE(def->parseInlineEdit != nullptr);

    auto const result = def->parseInlineEdit("");

    REQUIRE(result);

    auto const* value = std::get_if<std::uint16_t>(&*result);

    REQUIRE(value != nullptr);
    CHECK(*value == 0);
  }

  TEST_CASE("TrackFieldUi parseInlineEdit for invalid numeric text returns error", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::TrackNumber);

    REQUIRE(def != nullptr);
    REQUIRE(def->parseInlineEdit != nullptr);

    auto const result = def->parseInlineEdit("12a");

    REQUIRE_FALSE(result);
    CHECK(result.error().code == ao::Error::Code::FormatRejected);
  }

  TEST_CASE("TrackFieldUi writePatch for text fields sets optional string", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Title);

    REQUIRE(def != nullptr);
    REQUIRE(def->writePatch != nullptr);

    auto patch = MetadataPatch{};
    auto const value = Edit{std::in_place_type<std::string>, "New Title"};
    auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

    def->writePatch(ctx);

    REQUIRE(patch.optTitle.has_value());
    CHECK(*patch.optTitle == "New Title");
  }

  TEST_CASE("TrackFieldUi writePatch for artist sets optArtist", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Artist);

    REQUIRE(def != nullptr);
    REQUIRE(def->writePatch != nullptr);

    auto patch = MetadataPatch{};
    auto const value = Edit{std::in_place_type<std::string>, "New Artist"};
    auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

    def->writePatch(ctx);

    REQUIRE(patch.optArtist.has_value());
    CHECK(*patch.optArtist == "New Artist");
  }

  TEST_CASE("TrackFieldUi writePatch for album sets optAlbum", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Album);

    REQUIRE(def != nullptr);
    REQUIRE(def->writePatch != nullptr);

    auto patch = MetadataPatch{};
    auto const value = Edit{std::in_place_type<std::string>, "New Album"};
    auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

    def->writePatch(ctx);

    REQUIRE(patch.optAlbum.has_value());
    CHECK(*patch.optAlbum == "New Album");
  }

  TEST_CASE("TrackFieldUi writePatch for numeric fields sets uint16_t", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Year);

    REQUIRE(def != nullptr);
    REQUIRE(def->writePatch != nullptr);

    auto patch = MetadataPatch{};
    auto const value = Edit{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(1999)};
    auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

    def->writePatch(ctx);

    REQUIRE(patch.optYear.has_value());
    CHECK(*patch.optYear == 1999);
  }

  TEST_CASE("TrackFieldUi writePatch for track number", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::TrackNumber);

    REQUIRE(def != nullptr);
    REQUIRE(def->writePatch != nullptr);

    auto patch = MetadataPatch{};
    auto const value = Edit{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(7)};
    auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

    def->writePatch(ctx);

    REQUIRE(patch.optTrackNumber.has_value());
    CHECK(*patch.optTrackNumber == 7);
  }

  TEST_CASE("TrackFieldUi writePatch for disc number", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::DiscNumber);

    REQUIRE(def != nullptr);
    REQUIRE(def->writePatch != nullptr);

    auto patch = MetadataPatch{};
    auto const value = Edit{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(2)};
    auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

    def->writePatch(ctx);

    REQUIRE(patch.optDiscNumber.has_value());
    CHECK(*patch.optDiscNumber == 2);
  }

  TEST_CASE("TrackFieldUi all text metadata fields have writePatch", "[gtk][unit][trackfieldui]")
  {
    auto const textMetadataFields = {
      TrackField::Title,
      TrackField::Artist,
      TrackField::Album,
      TrackField::AlbumArtist,
      TrackField::Genre,
      TrackField::Composer,
      TrackField::Work,
    };

    for (auto const field : textMetadataFields)
    {
      auto const* def = trackFieldUiDefinition(field);

      INFO("Field: " << trackFieldId(field));
      REQUIRE(def != nullptr);
      CHECK(def->writePatch != nullptr);
      CHECK(def->parseInlineEdit != nullptr);
    }
  }

  TEST_CASE("TrackFieldUi all numeric metadata fields have writePatch", "[gtk][unit][trackfieldui]")
  {
    auto const numericMetadataFields = {
      TrackField::Year,
      TrackField::DiscNumber,
      TrackField::TotalDiscs,
      TrackField::TrackNumber,
      TrackField::TotalTracks,
    };

    for (auto const field : numericMetadataFields)
    {
      auto const* def = trackFieldUiDefinition(field);

      INFO("Field: " << trackFieldId(field));
      REQUIRE(def != nullptr);
      CHECK(def->writePatch != nullptr);
      CHECK(def->parseInlineEdit != nullptr);
    }
  }

  TEST_CASE("TrackFieldUi inline editable fields include key metadata columns", "[gtk][unit][trackfieldui]")
  {
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::Title)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::Artist)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::Album)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::AlbumArtist)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::Genre)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::Composer)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::Work)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::Year)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::DiscNumber)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::TotalDiscs)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::TrackNumber)));
    CHECK(canInlineEdit(*trackFieldUiDefinition(TrackField::TotalTracks)));

    CHECK_FALSE(canInlineEdit(*trackFieldUiDefinition(TrackField::Duration)));
  }

  TEST_CASE("TrackFieldUi inline editable fields have parser, writer, and edit hooks", "[gtk][unit][trackfieldui]")
  {
    for (auto const& def : trackFieldUiDefinitions())
    {
      if (!canInlineEdit(def))
      {
        continue;
      }

      INFO("Field: " << trackFieldId(def.field));
      CHECK(def.parseInlineEdit != nullptr);
      CHECK(def.writePatch != nullptr);
      CHECK(def.readRowEditValue != nullptr);
      CHECK(def.applyRowEditValue != nullptr);
    }
  }

  TEST_CASE("TrackFieldUi inline editing parse/apply for numeric fields", "[gtk][unit][trackfieldui]")
  {
    auto const fields = std::to_array({TrackField::Year,
                                       TrackField::DiscNumber,
                                       TrackField::TotalDiscs,
                                       TrackField::TrackNumber,
                                       TrackField::TotalTracks});

    for (auto const field : fields)
    {
      auto const* def = trackFieldUiDefinition(field);
      INFO("Field: " << trackFieldId(field));
      REQUIRE(def != nullptr);
      REQUIRE(def->parseInlineEdit != nullptr);

      SECTION("parse valid")
      {
        auto const result = def->parseInlineEdit("123");
        REQUIRE(result.has_value());
        auto const* val = std::get_if<std::uint16_t>(&result.value());
        REQUIRE(val != nullptr);
        CHECK(*val == 123);
      }

      SECTION("parse empty")
      {
        auto const result = def->parseInlineEdit("");
        REQUIRE(result.has_value());
        auto const* val = std::get_if<std::uint16_t>(&result.value());
        REQUIRE(val != nullptr);
        CHECK(*val == 0);
      }

      SECTION("parse invalid")
      {
        CHECK_FALSE(def->parseInlineEdit("abc").has_value());
        CHECK_FALSE(def->parseInlineEdit("-1").has_value());
        CHECK_FALSE(def->parseInlineEdit("70000").has_value());
      }
    }
  }

  TEST_CASE("TrackFieldUi readRowText is non-null for presentable fields", "[gtk][unit][trackfieldui]")
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

  TEST_CASE("TrackFieldUi readViewRawValue is non-null for non-synthetic fields", "[gtk][unit][trackfieldui]")
  {
    for (auto const& rtDef : kAllFields)
    {
      auto const* uiDef = trackFieldUiDefinition(rtDef.field);

      REQUIRE(uiDef != nullptr);

      if (rtDef.synthetic)
      {
        CHECK(uiDef->readViewRawValue == nullptr);
      }
      else
      {
        CHECK(uiDef->readViewRawValue != nullptr);
      }
    }
  }

  TEST_CASE("TrackFieldUi Tags has no writePatch", "[gtk][unit][trackfieldui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Tags);

    REQUIRE(def != nullptr);
    CHECK(def->writePatch == nullptr);
  }
} // namespace ao::gtk::test