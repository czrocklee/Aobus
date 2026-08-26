// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::winui::test
{
  TEST_CASE("TrackPropertiesAdapter - maps shared form rows without WinRT", "[winui][unit][track-properties]")
  {
    auto const row = uimodel::TrackPropertiesFormRow{
      .field = rt::TrackField::Title,
      .label = "Title",
      .editorKind = uimodel::TrackPropertiesFormEditorKind::Text,
    };

    SECTION("editable value")
    {
      auto const projected = projectTrackPropertyRow(row,
                                                     uimodel::TrackPropertiesFormRowView{
                                                       .field = rt::TrackField::Title,
                                                       .text = "曲名",
                                                       .mixed = false,
                                                       .editable = true,
                                                     });

      CHECK(projected.field == rt::TrackField::Title);
      CHECK(projected.label == "Title");
      CHECK(projected.text == "曲名");
      CHECK(projected.controlKind == TrackPropertyControlKind::Text);
      CHECK(projected.enabled);
      CHECK_FALSE(projected.mixed);
    }

    SECTION("mixed value remains visible but cannot be edited")
    {
      auto const projected = projectTrackPropertyRow(row,
                                                     uimodel::TrackPropertiesFormRowView{
                                                       .field = rt::TrackField::Title,
                                                       .text = "<Multiple Values>",
                                                       .mixed = true,
                                                       .editable = true,
                                                     });

      CHECK(projected.text == "<Multiple Values>");
      CHECK(projected.mixed);
      CHECK_FALSE(projected.enabled);
    }

    SECTION("number and readonly kinds stay distinct")
    {
      CHECK(trackPropertyControlKind(uimodel::TrackPropertiesFormEditorKind::Number) ==
            TrackPropertyControlKind::Number);
      CHECK(trackPropertyControlKind(uimodel::TrackPropertiesFormEditorKind::ReadonlyText) ==
            TrackPropertyControlKind::ReadonlyText);
    }
  }

  TEST_CASE("TrackPropertiesAdapter - parses native edits and command state", "[winui][unit][track-properties]")
  {
    auto const textRes = parseTrackPropertyEdit(TrackPropertyControlKind::Text, "Björk");
    REQUIRE(textRes);
    CHECK(std::get<std::string>(*textRes) == "Björk");

    auto const numberRes = parseTrackPropertyEdit(TrackPropertyControlKind::Number, " 2026 ");
    REQUIRE(numberRes);
    CHECK(std::get<std::uint16_t>(*numberRes) == 2026);

    CHECK_FALSE(parseTrackPropertyEdit(TrackPropertyControlKind::Number, "20x6"));
    CHECK_FALSE(parseTrackPropertyEdit(TrackPropertyControlKind::ReadonlyText, "ignored"));

    CHECK_FALSE(canPresentTrackProperties({}));
    CHECK(canPresentTrackProperties(std::array{TrackId{7}}));
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Applied) == TrackPropertiesCommitState::Accepted);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::NoOp) == TrackPropertiesCommitState::Accepted);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Busy) == TrackPropertiesCommitState::Busy);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Stale) == TrackPropertiesCommitState::Stale);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Unavailable) ==
          TrackPropertiesCommitState::Unavailable);
  }

  TEST_CASE("TrackPropertiesAdapter - projects tag and custom-key vocabulary", "[winui][unit][track-properties]")
  {
    auto const japaneseAliases = std::array<std::string, 1>{"yuduo"};
    auto const vocabulary = std::array{
      rt::VocabularyEntry{.value = "宇多田光", .frequency = 7, .aliases = japaneseAliases},
      rt::VocabularyEntry{.value = "Night Drive", .frequency = 4},
      rt::VocabularyEntry{.value = "Night", .frequency = 3},
    };

    CHECK(trackPropertyVocabularySuggestions(vocabulary, "night", 5) ==
          std::vector<std::string>{"Night Drive", "Night"});
    CHECK(trackPropertyVocabularySuggestions(vocabulary, "drive", 5) == std::vector<std::string>{"Night Drive"});
    CHECK(trackPropertyVocabularySuggestions(vocabulary, "yud", 5) == std::vector<std::string>{"宇多田光"});
    CHECK(trackPropertyVocabularySuggestions(vocabulary, "", 2) == std::vector<std::string>{"宇多田光", "Night Drive"});
  }

  TEST_CASE("TrackPropertiesAdapter - explicit empty custom value replaces a mixed original",
            "[winui][regression][track-properties]")
  {
    CHECK(customMetadataValueNeedsUpdate(true, std::nullopt, ""));
    CHECK(customMetadataValueNeedsUpdate(true, std::nullopt, "Ambient"));
    CHECK_FALSE(customMetadataValueNeedsUpdate(true, std::optional<std::string>{""}, ""));
    CHECK_FALSE(customMetadataValueNeedsUpdate(true, std::optional<std::string>{"Ambient"}, "Ambient"));
    CHECK(customMetadataValueNeedsUpdate(true, std::optional<std::string>{"Ambient"}, ""));
    CHECK(customMetadataValueNeedsUpdate(false, std::nullopt, ""));
  }
} // namespace ao::winui::test
