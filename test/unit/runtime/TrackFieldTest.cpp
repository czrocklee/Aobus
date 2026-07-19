// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
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
  constexpr auto kPersistedTrackFieldIds = std::to_array<std::string_view>({
    "title",
    "artist",
    "album",
    "album-artist",
    "genre",
    "composer",
    "conductor",
    "ensemble",
    "work",
    "movement",
    "soloist",
    "year",
    "disc-number",
    "disc-total",
    "track-number",
    "track-total",
    "movement-number",
    "movement-total",
    "duration",
    "tags",
    "file-path",
    "codec",
    "sample-rate",
    "channels",
    "bit-depth",
    "bitrate",
    "file-size",
    "modified-time",
    "display-track-number",
    "technical-summary",
    "quality",
  });

  constexpr auto kPersistedTrackSortFieldIds = std::to_array<std::string_view>({
    "artist",
    "album",
    "album-artist",
    "genre",
    "composer",
    "conductor",
    "ensemble",
    "work",
    "movement",
    "soloist",
    "year",
    "disc-number",
    "track-number",
    "title",
    "duration",
  });

  constexpr auto kPersistedTrackGroupKeyIds = std::to_array<std::string_view>({
    "none",
    "artist",
    "album",
    "album-artist",
    "genre",
    "composer",
    "conductor",
    "ensemble",
    "work",
    "year",
  });

  std::size_t countIf(auto const& range, auto const& predicate)
  {
    return static_cast<std::size_t>(std::ranges::count_if(range, predicate));
  }
} // namespace

namespace ao::rt::test
{
  TEST_CASE("TrackField - registry contains exactly kTrackFieldCount definitions", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    CHECK(defs.size() == kTrackFieldCount);
  }

  TEST_CASE("TrackField - registry has no duplicate ids", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();
    auto seen = std::unordered_set<std::string_view>{};

    for (auto const& def : defs)
    {
      INFO("Duplicate id: " << def.id);
      CHECK(seen.insert(def.id).second);
    }
  }

  TEST_CASE("TrackField - registry has no duplicate field values", "[runtime][unit][trackfield]")
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
    static_assert(kPersistedTrackFieldIds.size() == kTrackFieldCount);

    for (std::size_t index = 0; index < kPersistedTrackFieldIds.size(); ++index)
    {
      auto const field = static_cast<TrackField>(index);
      auto const id = trackFieldId(field);
      auto const optParsed = trackFieldFromId(id);

      INFO("Field: " << kPersistedTrackFieldIds[index]);
      CHECK(id == kPersistedTrackFieldIds[index]);
      REQUIRE(optParsed);
      CHECK(*optParsed == field);
    }
  }

  TEST_CASE("TrackField - persisted sort and group ids round-trip exhaustively", "[runtime][unit][trackfield]")
  {
    static_assert(kPersistedTrackSortFieldIds.size() == kTrackSortFieldCount);
    static_assert(kPersistedTrackGroupKeyIds.size() == kTrackGroupKeyCount);

    auto sortIds = std::unordered_set<std::string_view>{};

    for (std::size_t index = 0; index < kPersistedTrackSortFieldIds.size(); ++index)
    {
      auto const field = static_cast<TrackSortField>(index);
      auto const id = trackSortFieldId(field);
      auto const optParsed = trackSortFieldFromId(id);

      INFO("Sort field: " << kPersistedTrackSortFieldIds[index]);
      CHECK(id == kPersistedTrackSortFieldIds[index]);
      CHECK(sortIds.insert(id).second);
      REQUIRE(optParsed);
      CHECK(*optParsed == field);
    }

    auto groupIds = std::unordered_set<std::string_view>{};

    for (std::size_t index = 0; index < kPersistedTrackGroupKeyIds.size(); ++index)
    {
      auto const key = static_cast<TrackGroupKey>(index);
      auto const id = trackGroupKeyId(key);
      auto const optParsed = trackGroupKeyFromId(id);

      INFO("Group key: " << kPersistedTrackGroupKeyIds[index]);
      CHECK(id == kPersistedTrackGroupKeyIds[index]);
      CHECK(groupIds.insert(id).second);
      REQUIRE(optParsed);
      CHECK(*optParsed == key);
    }

    CHECK_FALSE(trackSortFieldFromId("AlbumArtist"));
    CHECK_FALSE(trackGroupKeyFromId("flat"));
    CHECK(trackSortFieldId(static_cast<TrackSortField>(255)).empty());
    CHECK(trackGroupKeyId(static_cast<TrackGroupKey>(255)).empty());
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

  TEST_CASE("TrackField - registry exposes expected category counts", "[runtime][unit][trackfield]")
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

  TEST_CASE("TrackField - registry marks all fields presentable", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    for (auto const& def : defs)
    {
      INFO("Field: " << def.id);
      CHECK(def.presentable);
    }
  }

  TEST_CASE("TrackField - editable fields match metadata text and numeric fields", "[runtime][unit][trackfield]")
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

  TEST_CASE("TrackField - sortable fields have valid optSortField", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    for (auto const& def : defs)
    {
      INFO("Field: " << def.id);

      if (def.sortable)
      {
        REQUIRE(def.optSortField);
        CHECK(static_cast<std::size_t>(*def.optSortField) < kTrackSortFieldCount);
      }
      else
      {
        CHECK_FALSE(def.optSortField);
      }
    }
  }

  TEST_CASE("TrackField - groupable fields have valid optGroupKey", "[runtime][unit][trackfield]")
  {
    auto const defs = trackFieldDefinitions();

    for (auto const& def : defs)
    {
      INFO("Field: " << def.id);

      if (def.groupable)
      {
        REQUIRE(def.optGroupKey);
        CHECK(static_cast<std::size_t>(*def.optGroupKey) < kTrackGroupKeyCount);
      }
      else
      {
        CHECK_FALSE(def.optGroupKey);
      }
    }
  }

  TEST_CASE("TrackField - synthetic fields are DisplayTrackNumber, TechnicalSummary, Quality",
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

  TEST_CASE("TrackField - definitions map fields to sort keys", "[runtime][unit][trackfield]")
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

  TEST_CASE("TrackField - definitions map fields to group keys", "[runtime][unit][trackfield]")
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

  TEST_CASE("TrackField - definitions expose filter expression variables", "[runtime][unit][trackfield]")
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
    CHECK(trackFieldFilterExpressionVariable(TrackField::Codec) == "@codec");
    CHECK(trackFieldFilterExpressionVariable(TrackField::SampleRate) == "@sampleRate");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Channels) == "@channels");
    CHECK(trackFieldFilterExpressionVariable(TrackField::BitDepth) == "@bitDepth");
    CHECK(trackFieldFilterExpressionVariable(TrackField::Bitrate) == "@bitrate");
  }

  TEST_CASE("TrackField - helpers report filter expression support", "[runtime][unit][trackfield]")
  {
    CHECK(supportsTrackFieldFilterExpression(TrackField::Artist));
    CHECK(supportsTrackFieldFilterExpression(TrackField::AlbumArtist));
    CHECK(supportsTrackFieldFilterExpression(TrackField::Duration));
    CHECK(supportsTrackFieldFilterExpression(TrackField::Codec));

    CHECK_FALSE(supportsTrackFieldFilterExpression(TrackField::Tags));
    CHECK_FALSE(supportsTrackFieldFilterExpression(TrackField::TechnicalSummary));
    CHECK_FALSE(supportsTrackFieldFilterExpression(TrackField::Quality));
  }

  TEST_CASE("TrackField - helpers report value completion support", "[runtime][unit][trackfield]")
  {
    CHECK(supportsTrackFieldValueCompletion(TrackField::Artist));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Album));
    CHECK(supportsTrackFieldValueCompletion(TrackField::AlbumArtist));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Genre));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Composer));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Conductor));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Ensemble));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Work));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Movement));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Soloist));

    CHECK_FALSE(supportsTrackFieldValueCompletion(TrackField::Title));
    CHECK_FALSE(supportsTrackFieldValueCompletion(TrackField::Year));
    CHECK_FALSE(supportsTrackFieldValueCompletion(TrackField::Tags));
  }

  TEST_CASE("TrackField - typed query bridges are exhaustive and unique", "[runtime][unit][trackfield]")
  {
    auto const definitions = trackFieldDefinitions();

    for (auto const type : {query::VariableType::Metadata, query::VariableType::Property})
    {
      for (auto const& descriptor : query::queryVariableDescriptors(type))
      {
        auto const optTrackField = trackFieldFromQueryField(descriptor.field);

        if (descriptor.field == query::Field::CoverArtId)
        {
          CHECK_FALSE(optTrackField);
          continue;
        }

        INFO("Missing runtime bridge for query variable: " << descriptor.canonicalName);
        REQUIRE(optTrackField);
        CHECK(trackFieldQueryField(*optTrackField) == descriptor.field);
      }
    }

    for (auto const& definition : definitions)
    {
      INFO("Invalid query bridge for track field: " << definition.id);
      CHECK(supportsTrackFieldFilterExpression(definition.field) == definition.optQueryField.has_value());
      CHECK(supportsTrackFieldValueCompletion(definition.field) == definition.valueCompletion);

      if (definition.valueCompletion)
      {
        REQUIRE(definition.optQueryField);
        CHECK(query::isDictionaryField(*definition.optQueryField));
      }

      if (!definition.optQueryField)
      {
        CHECK(trackFieldFilterExpressionVariable(definition.field).empty());
        continue;
      }

      CHECK(query::findQueryVariableDescriptor(*definition.optQueryField) != nullptr);
      CHECK_FALSE(trackFieldFilterExpressionVariable(definition.field).empty());
      CHECK(countIf(definitions,
                    [&definition](TrackFieldDefinition const& candidate)
                    { return candidate.optQueryField == definition.optQueryField; }) == 1);
    }
  }

  TEST_CASE("TrackField - helpers return empty values for invalid fields", "[runtime][unit][trackfield]")
  {
    auto const invalidField = static_cast<TrackField>(255);
    CHECK(trackFieldId(invalidField).empty());
    CHECK_FALSE(trackFieldQueryField(invalidField));
    CHECK(trackFieldFilterExpressionVariable(invalidField).empty());
    CHECK_FALSE(trackFieldFromQueryField(query::Field::Uri));
  }
} // namespace ao::rt::test
