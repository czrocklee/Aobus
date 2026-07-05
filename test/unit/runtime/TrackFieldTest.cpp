// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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

namespace ao::rt::test
{
  TEST_CASE("TrackField registry contains exactly kTrackFieldCount definitions", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    CHECK(defs.size() == kTrackFieldCount);
  }

  TEST_CASE("TrackField registry has no duplicate ids", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();
    auto seen = std::unordered_set<std::string_view>{};

    for (auto const& def : defs)
    {
      INFO("Duplicate id: " << def.id);
      CHECK(seen.insert(def.id).second);
    }
  }

  TEST_CASE("TrackField registry has no duplicate field values", "[runtime][unit][trackfield]")
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

  TEST_CASE("trackFieldArrayAt indexes fixed arrays by TrackField", "[runtime][unit][trackfield]")
  {
    auto labels = std::array<std::string_view, kTrackFieldCount>{};

    trackFieldArrayAt(labels, TrackField::Title) = "title";
    trackFieldArrayAt(labels, TrackField::Quality) = "quality";

    auto const& constLabels = labels;

    CHECK(trackFieldArrayAt(constLabels, TrackField::Title) == "title");
    CHECK(trackFieldArrayAt(constLabels, TrackField::Quality) == "quality");
  }

  TEST_CASE("trackFieldId round-trips through trackFieldFromId for all fields", "[runtime][unit][trackfield]")
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

  TEST_CASE("trackFieldDefinition returns non-null for all registered fields", "[runtime][unit][trackfield]")
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

  TEST_CASE("trackFieldFromId returns nullopt for unknown and empty ids", "[runtime][unit][trackfield]")
  {
    CHECK_FALSE(trackFieldFromId("bpm").has_value());
    CHECK_FALSE(trackFieldFromId("unknown-field").has_value());
    CHECK_FALSE(trackFieldFromId("").has_value());
    CHECK_FALSE(trackFieldFromId("Track").has_value());
  }

  TEST_CASE("TrackField registry exposes expected category counts", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    auto const metadataCount = countIf(defs, [](auto const& d) { return d.category == TrackFieldCategory::Metadata; });
    auto const tagCount = countIf(defs, [](auto const& d) { return d.category == TrackFieldCategory::Tag; });
    auto const technicalCount =
      countIf(defs, [](auto const& d) { return d.category == TrackFieldCategory::Technical; });
    auto const syntheticCount =
      countIf(defs, [](auto const& d) { return d.category == TrackFieldCategory::Synthetic; });

    CHECK(metadataCount == 18);
    CHECK(tagCount == 1);
    CHECK(technicalCount == 9);
    CHECK(syntheticCount == 3);
  }

  TEST_CASE("TrackField registry marks all fields presentable", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    for (auto const& def : defs)
    {
      INFO("Field: " << def.id);
      CHECK(def.presentable);
    }
  }

  TEST_CASE("TrackField editable fields match metadata text and numeric fields", "[runtime][unit][trackfield]")
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

  TEST_CASE("TrackField sortable fields have valid optSortField", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    for (auto const& def : defs)
    {
      INFO("Field: " << def.id);

      if (def.sortable)
      {
        REQUIRE(static_cast<bool>(def.optSortField));
        CHECK(static_cast<std::size_t>(*def.optSortField) < kTrackSortFieldCount);
      }
      else
      {
        CHECK_FALSE(static_cast<bool>(def.optSortField));
      }
    }
  }

  TEST_CASE("TrackField groupable fields have valid optGroupKey", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    for (auto const& def : defs)
    {
      INFO("Field: " << def.id);

      if (def.groupable)
      {
        REQUIRE(static_cast<bool>(def.optGroupKey));
        CHECK(static_cast<std::size_t>(*def.optGroupKey) < kTrackGroupKeyCount);
      }
      else
      {
        CHECK_FALSE(static_cast<bool>(def.optGroupKey));
      }
    }
  }

  TEST_CASE("TrackField synthetic fields are DisplayTrackNumber, TechnicalSummary, Quality",
            "[runtime][unit][trackfield]")
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

  TEST_CASE("TrackField definitions map fields to sort keys", "[runtime][unit][trackfield]")
  {
    CHECK(trackFieldDefinition(TrackField::Title)->optSortField == TrackSortField::Title);
    CHECK(trackFieldDefinition(TrackField::Artist)->optSortField == TrackSortField::Artist);
    CHECK(trackFieldDefinition(TrackField::Album)->optSortField == TrackSortField::Album);
    CHECK(trackFieldDefinition(TrackField::AlbumArtist)->optSortField == TrackSortField::AlbumArtist);
    CHECK(trackFieldDefinition(TrackField::Genre)->optSortField == TrackSortField::Genre);
    CHECK(trackFieldDefinition(TrackField::Composer)->optSortField == TrackSortField::Composer);
    CHECK(trackFieldDefinition(TrackField::Conductor)->optSortField == TrackSortField::Conductor);
    CHECK(trackFieldDefinition(TrackField::Ensemble)->optSortField == TrackSortField::Ensemble);
    CHECK(trackFieldDefinition(TrackField::Work)->optSortField == TrackSortField::Work);
    CHECK(trackFieldDefinition(TrackField::Movement)->optSortField == TrackSortField::Movement);
    CHECK(trackFieldDefinition(TrackField::Soloist)->optSortField == TrackSortField::Soloist);
    CHECK(trackFieldDefinition(TrackField::MovementNumber)->optSortField == TrackSortField::Movement);
    CHECK(trackFieldDefinition(TrackField::Year)->optSortField == TrackSortField::Year);
    CHECK(trackFieldDefinition(TrackField::DiscNumber)->optSortField == TrackSortField::DiscNumber);
    CHECK(trackFieldDefinition(TrackField::TrackNumber)->optSortField == TrackSortField::TrackNumber);
    CHECK(trackFieldDefinition(TrackField::Duration)->optSortField == TrackSortField::Duration);
  }

  TEST_CASE("TrackField definitions map fields to group keys", "[runtime][unit][trackfield]")
  {
    CHECK(trackFieldDefinition(TrackField::Artist)->optGroupKey == TrackGroupKey::Artist);
    CHECK(trackFieldDefinition(TrackField::Album)->optGroupKey == TrackGroupKey::Album);
    CHECK(trackFieldDefinition(TrackField::AlbumArtist)->optGroupKey == TrackGroupKey::AlbumArtist);
    CHECK(trackFieldDefinition(TrackField::Genre)->optGroupKey == TrackGroupKey::Genre);
    CHECK(trackFieldDefinition(TrackField::Composer)->optGroupKey == TrackGroupKey::Composer);
    CHECK(trackFieldDefinition(TrackField::Conductor)->optGroupKey == TrackGroupKey::Conductor);
    CHECK(trackFieldDefinition(TrackField::Ensemble)->optGroupKey == TrackGroupKey::Ensemble);
    CHECK(trackFieldDefinition(TrackField::Work)->optGroupKey == TrackGroupKey::Work);
    CHECK_FALSE(trackFieldDefinition(TrackField::Soloist)->optGroupKey);
    CHECK(trackFieldDefinition(TrackField::Year)->optGroupKey == TrackGroupKey::Year);
  }

  TEST_CASE("TrackField definitions expose stable labels", "[runtime][unit][trackfield]")
  {
    CHECK(trackFieldDefinition(TrackField::Title)->label == "Title");
    CHECK(trackFieldDefinition(TrackField::Artist)->label == "Artist");
    CHECK(trackFieldDefinition(TrackField::Album)->label == "Album");
    CHECK(trackFieldDefinition(TrackField::AlbumArtist)->label == "Album Artist");
    CHECK(trackFieldDefinition(TrackField::Genre)->label == "Genre");
    CHECK(trackFieldDefinition(TrackField::Composer)->label == "Composer");
    CHECK(trackFieldDefinition(TrackField::Conductor)->label == "Conductor");
    CHECK(trackFieldDefinition(TrackField::Ensemble)->label == "Ensemble");
    CHECK(trackFieldDefinition(TrackField::Work)->label == "Work");
    CHECK(trackFieldDefinition(TrackField::Movement)->label == "Movement");
    CHECK(trackFieldDefinition(TrackField::Soloist)->label == "Soloist");
    CHECK(trackFieldDefinition(TrackField::MovementNumber)->label == "Movement No.");
    CHECK(trackFieldDefinition(TrackField::MovementTotal)->label == "Total Movements");
    CHECK(trackFieldDefinition(TrackField::Year)->label == "Year");
    CHECK(trackFieldDefinition(TrackField::DiscNumber)->label == "Disc");
    CHECK(trackFieldDefinition(TrackField::DiscTotal)->label == "Total Discs");
    CHECK(trackFieldDefinition(TrackField::TrackNumber)->label == "Track");
    CHECK(trackFieldDefinition(TrackField::TrackTotal)->label == "Total Tracks");
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

  TEST_CASE("TrackField definitions expose filter expression variables", "[runtime][unit][trackfield]")
  {
    CHECK(trackFieldFilterExpressionVariable(TrackField::Title) == "$title");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Artist) == "$artist");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Album) == "$album");
    CHECK(trackFieldFilterExpressionVariable(TrackField::AlbumArtist) == "$albumArtist");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Genre) == "$genre");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Composer) == "$composer");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Conductor) == "$conductor");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Ensemble) == "$ensemble");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Work) == "$work");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Movement) == "$movement");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Soloist) == "$soloist");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Year) == "$year");
    CHECK(trackFieldFilterExpressionVariable(TrackField::DiscNumber) == "$discNumber");
    CHECK(trackFieldFilterExpressionVariable(TrackField::DiscTotal) == "$discTotal");
    CHECK(trackFieldFilterExpressionVariable(TrackField::TrackNumber) == "$trackNumber");
    CHECK(trackFieldFilterExpressionVariable(TrackField::TrackTotal) == "$trackTotal");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Duration) == "@duration");
    CHECK(trackFieldFilterExpressionVariable(TrackField::SampleRate) == "@sampleRate");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Channels) == "@channels");
    CHECK(trackFieldFilterExpressionVariable(TrackField::BitDepth) == "@bitDepth");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Bitrate) == "@bitrate");
  }

  TEST_CASE("TrackField helpers report filter expression support", "[runtime][unit][trackfield]")
  {
    CHECK(trackFieldSupportsFilterExpression(TrackField::Artist));
    CHECK(trackFieldSupportsFilterExpression(TrackField::AlbumArtist));
    CHECK(trackFieldSupportsFilterExpression(TrackField::Duration));

    CHECK_FALSE(trackFieldSupportsFilterExpression(TrackField::Tags));
    CHECK_FALSE(trackFieldSupportsFilterExpression(TrackField::TechnicalSummary));
    CHECK_FALSE(trackFieldSupportsFilterExpression(TrackField::Quality));
  }

  TEST_CASE("TrackField helpers report value completion support", "[runtime][unit][trackfield]")
  {
    CHECK(trackFieldSupportsValueCompletion(TrackField::Artist));
    CHECK(trackFieldSupportsValueCompletion(TrackField::Album));
    CHECK(trackFieldSupportsValueCompletion(TrackField::AlbumArtist));
    CHECK(trackFieldSupportsValueCompletion(TrackField::Genre));
    CHECK(trackFieldSupportsValueCompletion(TrackField::Composer));
    CHECK(trackFieldSupportsValueCompletion(TrackField::Conductor));
    CHECK(trackFieldSupportsValueCompletion(TrackField::Ensemble));
    CHECK(trackFieldSupportsValueCompletion(TrackField::Work));
    CHECK(trackFieldSupportsValueCompletion(TrackField::Movement));
    CHECK(trackFieldSupportsValueCompletion(TrackField::Soloist));

    CHECK_FALSE(trackFieldSupportsValueCompletion(TrackField::Title));
    CHECK_FALSE(trackFieldSupportsValueCompletion(TrackField::Year));
    CHECK_FALSE(trackFieldSupportsValueCompletion(TrackField::Tags));
  }

  TEST_CASE("TrackField helpers return empty values for invalid fields", "[runtime][unit][trackfield]")
  {
    auto const invalidField = static_cast<TrackField>(255);
    CHECK(trackFieldId(invalidField).empty());
    CHECK(trackFieldFilterExpressionVariable(invalidField).empty());
  }
} // namespace ao::rt::test
