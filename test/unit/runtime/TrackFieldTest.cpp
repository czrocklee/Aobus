// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_set>

namespace
{
  std::size_t countIf(auto const& range, auto const& predicate)
  {
    return static_cast<std::size_t>(std::ranges::count_if(range, predicate));
  }
}

using namespace ao::rt;

TEST_CASE("TrackField registry contains exactly kTrackFieldCount definitions", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  REQUIRE(defs.size() == kTrackFieldCount);
}

TEST_CASE("TrackField registry has no duplicate ids", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();
  auto seen = std::unordered_set<std::string_view>{};

  for (auto const& def : defs)
  {
    INFO("Duplicate id: " << def.id);
    CHECK(seen.insert(def.id).second);
  }
}

TEST_CASE("TrackField registry has no duplicate field values", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();
  auto seen = std::unordered_set<std::uint8_t>{};

  for (auto const& def : defs)
  {
    auto const raw = static_cast<std::uint8_t>(def.field);

    INFO("Duplicate field value: " << static_cast<int>(raw));
    CHECK(seen.insert(raw).second);
  }
}

TEST_CASE("trackFieldId round-trips through trackFieldFromId for all fields", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  for (auto const& def : defs)
  {
    auto const id = trackFieldId(def.field);
    auto const optParsed = trackFieldFromId(id);

    INFO("Field: " << def.id);
    REQUIRE(optParsed.has_value());
    CHECK(*optParsed == def.field);
  }
}

TEST_CASE("trackFieldDefinition returns non-null for all registered fields", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  for (auto const& def : defs)
  {
    INFO("Field: " << def.id);
    auto const* lookup = trackFieldDefinition(def.field);

    REQUIRE(lookup != nullptr);
    CHECK(lookup->field == def.field);
    CHECK(lookup->id == def.id);
  }
}

TEST_CASE("trackFieldFromId returns nullopt for unknown and empty ids", "[runtime][trackfield]")
{
  CHECK_FALSE(trackFieldFromId("bpm").has_value());
  CHECK_FALSE(trackFieldFromId("unknown-field").has_value());
  CHECK_FALSE(trackFieldFromId("").has_value());
  CHECK_FALSE(trackFieldFromId("Track").has_value());
}

TEST_CASE("TrackField category counts are correct", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  auto const metadataCount = countIf(defs, [](auto const& d) { return d.category == TrackFieldCategory::Metadata; });
  auto const tagCount = countIf(defs, [](auto const& d) { return d.category == TrackFieldCategory::Tag; });
  auto const technicalCount = countIf(defs, [](auto const& d) { return d.category == TrackFieldCategory::Technical; });
  auto const syntheticCount = countIf(defs, [](auto const& d) { return d.category == TrackFieldCategory::Synthetic; });

  CHECK(metadataCount == 12);
  CHECK(tagCount == 1);
  CHECK(technicalCount == 9);
  CHECK(syntheticCount == 3);
}

TEST_CASE("TrackField presentable fields are correct", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  for (auto const& def : defs)
  {
    INFO("Field: " << def.id);
    CHECK(def.presentable);
  }
}

TEST_CASE("TrackField editable fields match metadata text and numeric fields", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  for (auto const& def : defs)
  {
    INFO("Field: " << def.id);

    if (def.category == TrackFieldCategory::Metadata)
    {
      CHECK(def.editable);
    }
    else
    {
      CHECK_FALSE(def.editable);
    }
  }
}

TEST_CASE("TrackField sortable fields have valid optSortField", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  for (auto const& def : defs)
  {
    INFO("Field: " << def.id);

    if (def.sortable)
    {
      REQUIRE(static_cast<bool>(def.optSortField));
      CHECK(static_cast<std::size_t>(*def.optSortField) < 11);
    }
    else
    {
      CHECK_FALSE(static_cast<bool>(def.optSortField));
    }
  }
}

TEST_CASE("TrackField groupable fields have valid optGroupKey", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  for (auto const& def : defs)
  {
    INFO("Field: " << def.id);

    if (def.groupable)
    {
      REQUIRE(static_cast<bool>(def.optGroupKey));
      CHECK(static_cast<std::size_t>(*def.optGroupKey) < 9);
    }
    else
    {
      CHECK_FALSE(static_cast<bool>(def.optGroupKey));
    }
  }
}

TEST_CASE("TrackField synthetic fields are DisplayTrackNumber, TechnicalSummary, Quality", "[runtime][trackfield]")
{
  auto const defs = trackFieldDefinitions();

  for (auto const& def : defs)
  {
    INFO("Field: " << def.id);

    if (def.field == TrackField::DisplayTrackNumber || def.field == TrackField::TechnicalSummary ||
        def.field == TrackField::Quality)
    {
      CHECK(def.synthetic);
    }
    else
    {
      CHECK_FALSE(def.synthetic);
    }
  }
}

TEST_CASE("TrackField known sort fields", "[runtime][trackfield]")
{
  CHECK(trackFieldDefinition(TrackField::Title)->optSortField == TrackSortField::Title);
  CHECK(trackFieldDefinition(TrackField::Artist)->optSortField == TrackSortField::Artist);
  CHECK(trackFieldDefinition(TrackField::Album)->optSortField == TrackSortField::Album);
  CHECK(trackFieldDefinition(TrackField::AlbumArtist)->optSortField == TrackSortField::AlbumArtist);
  CHECK(trackFieldDefinition(TrackField::Genre)->optSortField == TrackSortField::Genre);
  CHECK(trackFieldDefinition(TrackField::Composer)->optSortField == TrackSortField::Composer);
  CHECK(trackFieldDefinition(TrackField::Work)->optSortField == TrackSortField::Work);
  CHECK(trackFieldDefinition(TrackField::Year)->optSortField == TrackSortField::Year);
  CHECK(trackFieldDefinition(TrackField::DiscNumber)->optSortField == TrackSortField::DiscNumber);
  CHECK(trackFieldDefinition(TrackField::TrackNumber)->optSortField == TrackSortField::TrackNumber);
  CHECK(trackFieldDefinition(TrackField::Duration)->optSortField == TrackSortField::Duration);
}

TEST_CASE("TrackField known group keys", "[runtime][trackfield]")
{
  CHECK(trackFieldDefinition(TrackField::Artist)->optGroupKey == TrackGroupKey::Artist);
  CHECK(trackFieldDefinition(TrackField::Album)->optGroupKey == TrackGroupKey::Album);
  CHECK(trackFieldDefinition(TrackField::AlbumArtist)->optGroupKey == TrackGroupKey::AlbumArtist);
  CHECK(trackFieldDefinition(TrackField::Genre)->optGroupKey == TrackGroupKey::Genre);
  CHECK(trackFieldDefinition(TrackField::Composer)->optGroupKey == TrackGroupKey::Composer);
  CHECK(trackFieldDefinition(TrackField::Work)->optGroupKey == TrackGroupKey::Work);
  CHECK(trackFieldDefinition(TrackField::Year)->optGroupKey == TrackGroupKey::Year);
}

TEST_CASE("TrackField known labels", "[runtime][trackfield]")
{
  CHECK(trackFieldDefinition(TrackField::Title)->label == "Title");
  CHECK(trackFieldDefinition(TrackField::Artist)->label == "Artist");
  CHECK(trackFieldDefinition(TrackField::Album)->label == "Album");
  CHECK(trackFieldDefinition(TrackField::AlbumArtist)->label == "Album Artist");
  CHECK(trackFieldDefinition(TrackField::Genre)->label == "Genre");
  CHECK(trackFieldDefinition(TrackField::Composer)->label == "Composer");
  CHECK(trackFieldDefinition(TrackField::Work)->label == "Work");
  CHECK(trackFieldDefinition(TrackField::Year)->label == "Year");
  CHECK(trackFieldDefinition(TrackField::DiscNumber)->label == "Disc");
  CHECK(trackFieldDefinition(TrackField::TotalDiscs)->label == "Total Discs");
  CHECK(trackFieldDefinition(TrackField::TrackNumber)->label == "Track");
  CHECK(trackFieldDefinition(TrackField::TotalTracks)->label == "Total Tracks");
  CHECK(trackFieldDefinition(TrackField::Duration)->label == "Duration");
  CHECK(trackFieldDefinition(TrackField::Tags)->label == "Tags");
  CHECK(trackFieldDefinition(TrackField::FilePath)->label == "File Path");
  CHECK(trackFieldDefinition(TrackField::Codec)->label == "Codec");
  CHECK(trackFieldDefinition(TrackField::SampleRate)->label == "Sample Rate");
  CHECK(trackFieldDefinition(TrackField::Channels)->label == "Channels");
  CHECK(trackFieldDefinition(TrackField::BitDepth)->label == "Bit Depth");
  CHECK(trackFieldDefinition(TrackField::Bitrate)->label == "Bitrate");
  CHECK(trackFieldDefinition(TrackField::FileSize)->label == "File Size");
  CHECK(trackFieldDefinition(TrackField::ModifiedTime)->label == "Modified");
  CHECK(trackFieldDefinition(TrackField::DisplayTrackNumber)->label == "Track #");
  CHECK(trackFieldDefinition(TrackField::TechnicalSummary)->label == "Technical");
  CHECK(trackFieldDefinition(TrackField::Quality)->label == "Quality");
}
