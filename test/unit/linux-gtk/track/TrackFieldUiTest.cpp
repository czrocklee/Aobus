// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/track/TrackFieldUi.h"

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace ao::gtk;
using namespace ao::rt;

namespace
{
  using Raw = detail::TrackFieldRawValue;
  using Dur = detail::Duration;

  auto const kAllFields = trackFieldDefinitions();
} // namespace

TEST_CASE("TrackFieldUi registry has entry for every runtime field", "[gtk][trackfieldui]")
{
  for (auto const& rtDef : kAllFields)
  {
    INFO("Field " << rtDef.id << " must have a UI definition");
    CHECK(trackFieldUiDefinition(rtDef.field) != nullptr);
  }
}

TEST_CASE("TrackFieldUi registry has kTrackFieldCount definitions", "[gtk][trackfieldui]")
{
  auto const uiDefs = trackFieldUiDefinitions();

  CHECK(uiDefs.size() == ao::rt::kTrackFieldCount);
}

TEST_CASE("TrackFieldUi formatValue for text fields returns raw string", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Title);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<std::string>, "Hello"});

  CHECK(formatted == "Hello");
}

TEST_CASE("TrackFieldUi formatValue for text returns empty for non-string variant", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Title);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::monostate{}});

  CHECK(formatted.empty());
}

TEST_CASE("TrackFieldUi formatValue for uint16_t returns formatted number", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Year);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(2024)});

  CHECK(formatted == "2024");
}

TEST_CASE("TrackFieldUi formatValue for uint16_t returns empty for zero", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::TrackNumber);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(0)});

  CHECK(formatted.empty());
}

TEST_CASE("TrackFieldUi formatValue for Duration produces M:SS format", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Duration);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<Dur>, Dur{225000}});

  CHECK(formatted == "3:45");
}

TEST_CASE("TrackFieldUi formatValue for Duration with hours", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Duration);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<Dur>, Dur{3723000}});

  CHECK(formatted == "1:2:03");
}

TEST_CASE("TrackFieldUi formatValue for Duration returns empty for zero", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Duration);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<Dur>, Dur{0}});

  CHECK(formatted.empty());
}

TEST_CASE("TrackFieldUi formatValue for SampleRate appends Hz", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::SampleRate);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(44100)});

  CHECK(formatted == "44100 Hz");
}

TEST_CASE("TrackFieldUi formatValue for Bitrate appends kbps", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Bitrate);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(320000)});

  CHECK(formatted == "320 kbps");
}

TEST_CASE("TrackFieldUi raw values distinguish formatted collisions", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Bitrate);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const low = Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(1000)};
  auto const high = Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(1999)};

  REQUIRE(def->formatValue(low) == def->formatValue(high));
  CHECK(low != high);
}

TEST_CASE("TrackFieldUi formatValue for Channels returns Mono/Stereo", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Channels);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  CHECK(def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(1)}) == "Mono");
  CHECK(def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(2)}) == "Stereo");
}

TEST_CASE("TrackFieldUi formatValue for FileSize", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::FileSize);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const mb = def->formatValue(Raw{std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(5242880)});

  CHECK(mb == "5.0 MB");
}

TEST_CASE("TrackFieldUi formatValue for BitDepth", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::BitDepth);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(24)});

  CHECK(formatted == "24-bit");
}

TEST_CASE("TrackFieldUi formatValue returns empty for non-matching variant", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Year);

  REQUIRE(def != nullptr);
  REQUIRE(def->formatValue != nullptr);

  auto const formatted = def->formatValue(Raw{std::in_place_type<std::string>, "not a number"});

  CHECK(formatted.empty());
}

TEST_CASE("TrackFieldUi writePatch for text fields sets optional string", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Title);

  REQUIRE(def != nullptr);
  REQUIRE(def->writePatch != nullptr);

  auto patch = MetadataPatch{};
  auto const value = Raw{std::in_place_type<std::string>, "New Title"};
  auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

  def->writePatch(ctx);

  REQUIRE(patch.optTitle.has_value());
  CHECK(*patch.optTitle == "New Title");
}

TEST_CASE("TrackFieldUi writePatch for artist sets optArtist", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Artist);

  REQUIRE(def != nullptr);
  REQUIRE(def->writePatch != nullptr);

  auto patch = MetadataPatch{};
  auto const value = Raw{std::in_place_type<std::string>, "New Artist"};
  auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

  def->writePatch(ctx);

  REQUIRE(patch.optArtist.has_value());
  CHECK(*patch.optArtist == "New Artist");
}

TEST_CASE("TrackFieldUi writePatch for album sets optAlbum", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Album);

  REQUIRE(def != nullptr);
  REQUIRE(def->writePatch != nullptr);

  auto patch = MetadataPatch{};
  auto const value = Raw{std::in_place_type<std::string>, "New Album"};
  auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

  def->writePatch(ctx);

  REQUIRE(patch.optAlbum.has_value());
  CHECK(*patch.optAlbum == "New Album");
}

TEST_CASE("TrackFieldUi writePatch for numeric fields sets uint16_t", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Year);

  REQUIRE(def != nullptr);
  REQUIRE(def->writePatch != nullptr);

  auto patch = MetadataPatch{};
  auto const value = Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(1999)};
  auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

  def->writePatch(ctx);

  REQUIRE(patch.optYear.has_value());
  CHECK(*patch.optYear == 1999);
}

TEST_CASE("TrackFieldUi writePatch for track number", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::TrackNumber);

  REQUIRE(def != nullptr);
  REQUIRE(def->writePatch != nullptr);

  auto patch = MetadataPatch{};
  auto const value = Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(7)};
  auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

  def->writePatch(ctx);

  REQUIRE(patch.optTrackNumber.has_value());
  CHECK(*patch.optTrackNumber == 7);
}

TEST_CASE("TrackFieldUi writePatch for disc number", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::DiscNumber);

  REQUIRE(def != nullptr);
  REQUIRE(def->writePatch != nullptr);

  auto patch = MetadataPatch{};
  auto const value = Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(2)};
  auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = value};

  def->writePatch(ctx);

  REQUIRE(patch.optDiscNumber.has_value());
  CHECK(*patch.optDiscNumber == 2);
}

TEST_CASE("TrackFieldUi all text metadata fields have writePatch", "[gtk][trackfieldui]")
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
    CHECK(def->propertyDialogEditable);
  }
}

TEST_CASE("TrackFieldUi all numeric metadata fields have writePatch", "[gtk][trackfieldui]")
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
    CHECK(def->propertyDialogEditable);
  }
}

TEST_CASE("TrackFieldUi drag query prefixes exist", "[gtk][trackfieldui]")
{
  CHECK(trackFieldUiDefinition(TrackField::Artist)->dragQueryPrefix == "$a=");
  CHECK(trackFieldUiDefinition(TrackField::Album)->dragQueryPrefix == "$al=");
  CHECK(trackFieldUiDefinition(TrackField::Genre)->dragQueryPrefix == "$g=");
}

TEST_CASE("TrackFieldUi drag prefix empty for non-draggable fields", "[gtk][trackfieldui]")
{
  CHECK(trackFieldUiDefinition(TrackField::Title)->dragQueryPrefix.empty());
  CHECK(trackFieldUiDefinition(TrackField::Duration)->dragQueryPrefix.empty());
  CHECK(trackFieldUiDefinition(TrackField::Year)->dragQueryPrefix.empty());
}

TEST_CASE("TrackFieldUi column visible by default for built-in fields", "[gtk][trackfieldui]")
{
  CHECK(trackFieldUiDefinition(TrackField::Title)->columnVisibleByDefault);
  CHECK(trackFieldUiDefinition(TrackField::Artist)->columnVisibleByDefault);
  CHECK(trackFieldUiDefinition(TrackField::Album)->columnVisibleByDefault);
  CHECK(trackFieldUiDefinition(TrackField::Duration)->columnVisibleByDefault);
  CHECK(trackFieldUiDefinition(TrackField::Tags)->columnVisibleByDefault);
}

TEST_CASE("TrackFieldUi column not visible by default for curated fields", "[gtk][trackfieldui]")
{
  CHECK_FALSE(trackFieldUiDefinition(TrackField::AlbumArtist)->columnVisibleByDefault);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Genre)->columnVisibleByDefault);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Composer)->columnVisibleByDefault);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Work)->columnVisibleByDefault);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Year)->columnVisibleByDefault);
}

TEST_CASE("TrackFieldUi column expanding only Tags", "[gtk][trackfieldui]")
{
  CHECK(trackFieldUiDefinition(TrackField::Tags)->columnExpands);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Title)->columnExpands);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Artist)->columnExpands);
}

TEST_CASE("TrackFieldUi column numeric fields", "[gtk][trackfieldui]")
{
  CHECK(trackFieldUiDefinition(TrackField::Year)->columnNumeric);
  CHECK(trackFieldUiDefinition(TrackField::DiscNumber)->columnNumeric);
  CHECK(trackFieldUiDefinition(TrackField::TrackNumber)->columnNumeric);
  CHECK(trackFieldUiDefinition(TrackField::Duration)->columnNumeric);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Title)->columnNumeric);
}

TEST_CASE("TrackFieldUi column tags cell only Tags", "[gtk][trackfieldui]")
{
  CHECK(trackFieldUiDefinition(TrackField::Tags)->columnTagsCell);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Title)->columnTagsCell);
}

TEST_CASE("TrackFieldUi inline editable only Title/Artist/Album", "[gtk][trackfieldui]")
{
  CHECK(trackFieldUiDefinition(TrackField::Title)->inlineEditable);
  CHECK(trackFieldUiDefinition(TrackField::Artist)->inlineEditable);
  CHECK(trackFieldUiDefinition(TrackField::Album)->inlineEditable);

  CHECK_FALSE(trackFieldUiDefinition(TrackField::AlbumArtist)->inlineEditable);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Genre)->inlineEditable);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Year)->inlineEditable);
  CHECK_FALSE(trackFieldUiDefinition(TrackField::Duration)->inlineEditable);
}

TEST_CASE("TrackFieldUi readRowText is non-null for presentable fields", "[gtk][trackfieldui]")
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

TEST_CASE("TrackFieldUi readViewRawValue is non-null for non-synthetic fields", "[gtk][trackfieldui]")
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

TEST_CASE("TrackFieldUi property dialog readonly fields", "[gtk][trackfieldui]")
{
  CHECK(trackFieldUiDefinition(TrackField::FilePath)->propertyDialogReadonly);
  CHECK(trackFieldUiDefinition(TrackField::Codec)->propertyDialogReadonly);
  CHECK(trackFieldUiDefinition(TrackField::SampleRate)->propertyDialogReadonly);
  CHECK(trackFieldUiDefinition(TrackField::Channels)->propertyDialogReadonly);
  CHECK(trackFieldUiDefinition(TrackField::BitDepth)->propertyDialogReadonly);
  CHECK(trackFieldUiDefinition(TrackField::Bitrate)->propertyDialogReadonly);
  CHECK(trackFieldUiDefinition(TrackField::Duration)->propertyDialogReadonly);
  CHECK(trackFieldUiDefinition(TrackField::FileSize)->propertyDialogReadonly);
  CHECK(trackFieldUiDefinition(TrackField::ModifiedTime)->propertyDialogReadonly);
}

TEST_CASE("TrackFieldUi Tags has no writePatch", "[gtk][trackfieldui]")
{
  auto const* def = trackFieldUiDefinition(TrackField::Tags);

  REQUIRE(def != nullptr);
  CHECK(def->writePatch == nullptr);
  CHECK_FALSE(def->propertyDialogEditable);
}
