// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackPropertiesAdapter.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::winui::test
{
  TEST_CASE("TrackPropertiesAdapter - maps shared form rows without WinRT", "[winui][unit][property]")
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
  }

  TEST_CASE("TrackPropertiesAdapter - number, date, and readonly control kinds stay distinct",
            "[winui][unit][property]")
  {
    CHECK(trackPropertyControlKind(uimodel::TrackPropertiesFormEditorKind::Number) == TrackPropertyControlKind::Number);
    CHECK(trackPropertyControlKind(uimodel::TrackPropertiesFormEditorKind::Date) == TrackPropertyControlKind::Date);
    CHECK(trackPropertyControlKind(uimodel::TrackPropertiesFormEditorKind::ReadonlyText) ==
          TrackPropertyControlKind::ReadonlyText);
  }

  TEST_CASE("TrackPropertiesAdapter - projects date rows like editable text metadata", "[winui][unit][property]")
  {
    auto const row = uimodel::TrackPropertiesFormRow{
      .field = rt::TrackField::RecordingDate,
      .label = "Recording date",
      .editorKind = uimodel::TrackPropertiesFormEditorKind::Date,
    };

    SECTION("editable value")
    {
      auto const projected = projectTrackPropertyRow(row,
                                                     uimodel::TrackPropertiesFormRowView{
                                                       .field = rt::TrackField::RecordingDate,
                                                       .text = "1981-05",
                                                       .mixed = false,
                                                       .editable = true,
                                                     });

      CHECK(projected.controlKind == TrackPropertyControlKind::Date);
      CHECK(projected.enabled);
      CHECK_FALSE(projected.mixed);
    }

    SECTION("mixed value stays visible but cannot be edited")
    {
      auto const projected = projectTrackPropertyRow(row,
                                                     uimodel::TrackPropertiesFormRowView{
                                                       .field = rt::TrackField::RecordingDate,
                                                       .text = "<Multiple Values>",
                                                       .mixed = true,
                                                       .editable = true,
                                                     });

      CHECK(projected.controlKind == TrackPropertyControlKind::Date);
      CHECK(projected.mixed);
      CHECK_FALSE(projected.enabled);
    }
  }

  TEST_CASE("TrackPropertiesAdapter - parses native edits", "[winui][unit][property]")
  {
    auto const textRes = parseTrackPropertyEdit(TrackPropertyControlKind::Text, "Björk");
    REQUIRE(textRes);
    CHECK(std::get<std::string>(*textRes) == "Björk");

    auto const numberRes = parseTrackPropertyEdit(TrackPropertyControlKind::Number, " 2026 ");
    REQUIRE(numberRes);
    CHECK(std::get<std::uint16_t>(*numberRes) == 2026);

    auto const fullDateRes = parseTrackPropertyEdit(TrackPropertyControlKind::Date, "1981-05-12");
    REQUIRE(fullDateRes);
    CHECK(std::get<library::RecordingDate>(*fullDateRes) ==
          library::RecordingDate{.year = 1981, .month = 5, .day = 12});

    auto const yearDateRes = parseTrackPropertyEdit(TrackPropertyControlKind::Date, " 1981 ");
    REQUIRE(yearDateRes);
    CHECK(std::get<library::RecordingDate>(*yearDateRes) == library::RecordingDate{.year = 1981});

    // An empty editor input explicitly clears the recording date; zero is the
    // shared absence sentinel, not an admitted year.
    auto const clearDateRes = parseTrackPropertyEdit(TrackPropertyControlKind::Date, " ");
    REQUIRE(clearDateRes);
    CHECK_FALSE(std::get<library::RecordingDate>(*clearDateRes).isPresent());

    CHECK_FALSE(parseTrackPropertyEdit(TrackPropertyControlKind::Number, "20x6"));
    CHECK_FALSE(parseTrackPropertyEdit(TrackPropertyControlKind::Date, "1981-13"));
    CHECK_FALSE(parseTrackPropertyEdit(TrackPropertyControlKind::Date, "81"));
    CHECK_FALSE(parseTrackPropertyEdit(TrackPropertyControlKind::ReadonlyText, "ignored"));
  }

  TEST_CASE("TrackPropertiesAdapter - presentation requires a nonempty selection", "[winui][unit][property]")
  {
    CHECK_FALSE(canPresentTrackProperties({}));
    CHECK(canPresentTrackProperties(std::array{TrackId{7}}));
  }

  TEST_CASE("TrackPropertiesAdapter - projects authoring commit states", "[winui][unit][property]")
  {
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Applied) == TrackPropertiesCommitState::Accepted);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::NoOp) == TrackPropertiesCommitState::Accepted);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Busy) == TrackPropertiesCommitState::Busy);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Stale) == TrackPropertiesCommitState::Stale);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Unavailable) ==
          TrackPropertiesCommitState::Unavailable);
  }

  TEST_CASE("TrackPropertiesAdapter - projects tag and custom-key vocabulary", "[winui][unit][property]")
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

  TEST_CASE("TrackPropertiesAdapter - explicit empty custom value replaces a mixed original", "[winui][unit][property]")
  {
    CHECK(needsCustomMetadataValueUpdate(true, std::nullopt, ""));
    CHECK(needsCustomMetadataValueUpdate(true, std::nullopt, "Ambient"));
    CHECK_FALSE(needsCustomMetadataValueUpdate(true, std::optional<std::string>{""}, ""));
    CHECK_FALSE(needsCustomMetadataValueUpdate(true, std::optional<std::string>{"Ambient"}, "Ambient"));
    CHECK(needsCustomMetadataValueUpdate(true, std::optional<std::string>{"Ambient"}, ""));
    CHECK(needsCustomMetadataValueUpdate(false, std::nullopt, ""));
  }

  TEST_CASE("TrackPropertiesAdapter - category previews never expose scalar credit edits", "[winui][unit][property]")
  {
    for (auto const field : {rt::TrackField::Conductor, rt::TrackField::Ensemble, rt::TrackField::Soloist})
    {
      auto const projected = projectTrackPropertyRow(
        {.field = field, .label = "Credit", .editorKind = uimodel::TrackPropertiesFormEditorKind::Text},
        {.field = field, .text = "First +2", .editable = true});
      CHECK(projected.controlKind == TrackPropertyControlKind::ReadonlyText);
      CHECK_FALSE(projected.enabled);
      CHECK(projected.text == "First +2");
    }

    CHECK(trackPropertyCreditKind(rt::TrackField::Conductor) == library::CreditKind::Conductor);
    CHECK(trackPropertyCreditKind(rt::TrackField::Ensemble) == library::CreditKind::Ensemble);
    CHECK(trackPropertyCreditKind(rt::TrackField::Soloist) == library::CreditKind::Soloist);
    CHECK_FALSE(trackPropertyCreditKind(rt::TrackField::Artist));
    CHECK_FALSE(trackPropertyCreditKind(rt::TrackField::Composer));
    CHECK_FALSE(trackPropertyCreditKind(static_cast<rt::TrackField>(255)));
    auto const composer =
      projectTrackPropertyRow({.field = rt::TrackField::Composer,
                               .label = "Composer",
                               .editorKind = uimodel::TrackPropertiesFormEditorKind::Text},
                              {.field = rt::TrackField::Composer, .text = "Bach", .editable = true});
    CHECK(composer.controlKind == TrackPropertyControlKind::Text);
    CHECK(composer.enabled);
    CHECK(composer.text == "Bach");
  }

  TEST_CASE("TrackPropertiesAdapter - active credit child blocks tags and ordinary field submission",
            "[winui][unit][property]")
  {
    auto form = uimodel::TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    form.addField(rt::TrackField::Title, true);
    form.loadFirstTrackField(rt::TrackField::Title, std::string{"Before"});
    form.setEditValue(rt::TrackField::Title, std::string{"After"});
    REQUIRE(canSubmitTrackProperties(form, false));
    auto sections = uimodel::TrackCreditSections{};
    sections[static_cast<std::size_t>(library::CreditKind::Soloist)].optValue = std::vector<library::Credit>{};
    REQUIRE(form.creditsEditor().begin(sections, uimodel::trackCreditScope(library::CreditKind::Soloist)));
    CHECK_FALSE(canSubmitTrackProperties(form, false));
    CHECK_FALSE(canSubmitTrackProperties(form, true));
    form.creditsEditor().addEntry(library::CreditKind::Soloist);
    CHECK_FALSE(form.creditsEditor().canCommit());
    CHECK_FALSE(canSubmitTrackProperties(form, true));
    form.cancelCreditsEdit();
    CHECK(canSubmitTrackProperties(form, false));
    CHECK(canSubmitTrackProperties(form, true));
    auto const patch = form.buildPatch();
    CHECK(patch.optTitle == "After");
    CHECK_FALSE(patch.optCredits);
  }

  TEST_CASE("TrackPropertiesAdapter - accepting credits stages one typed scope with the ordinary draft",
            "[winui][unit][property]")
  {
    auto form = uimodel::TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    form.addField(rt::TrackField::Title, true);
    form.loadFirstTrackField(rt::TrackField::Title, std::string{"Before"});
    form.setEditValue(rt::TrackField::Title, std::string{"After"});
    auto sections = uimodel::TrackCreditSections{};
    sections[static_cast<std::size_t>(library::CreditKind::Soloist)].optValue = std::vector<library::Credit>{};
    auto& editor = form.creditsEditor();
    REQUIRE(editor.begin(sections, uimodel::trackCreditScope(library::CreditKind::Soloist)));
    editor.addEntry(library::CreditKind::Soloist);
    editor.updateName(0, "Anne");
    editor.updateRole(0, "violin");
    REQUIRE(editor.canCommit());
    CHECK_FALSE(canSubmitTrackProperties(form, true));
    REQUIRE(form.acceptCreditsEdit());
    CHECK(canSubmitTrackProperties(form, false));
    auto const patch = form.buildPatch();
    CHECK(patch.optTitle == "After");
    REQUIRE(patch.optCredits);
    CHECK(patch.optCredits->kinds == uimodel::trackCreditScope(library::CreditKind::Soloist));
    CHECK(patch.optCredits->entries ==
          std::vector<library::Credit>{{.name = "Anne", .kind = library::CreditKind::Soloist, .role = "violin"}});
  }
} // namespace ao::winui::test
