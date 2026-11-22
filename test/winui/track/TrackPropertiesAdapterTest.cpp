// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackPropertiesAdapter.h>

#include "test/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
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
  TEST_CASE("TrackPropertiesAdapter - maps shared form rows without WinRT")
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
      CHECK(projected.controlKind == uimodel::TrackPropertiesFormEditorKind::Text);
      CHECK(projected.enabled);
      CHECK(projected.placeholder.empty());
    }

    SECTION("mixed markers are placeholders rather than native input")
    {
      auto const projected = projectTrackPropertyRow(row,
                                                     uimodel::TrackPropertiesFormRowView{
                                                       .field = rt::TrackField::Title,
                                                       .text = "<Multiple Values>",
                                                       .mixed = true,
                                                       .editable = true,
                                                     });

      CHECK(projected.placeholder == "<Multiple Values>");
      CHECK(projected.text.empty());
      CHECK(projected.enabled);
    }
  }

  TEST_CASE("TrackPropertiesAdapter - batch album inputs preserve until replaced or explicitly cleared")
  {
    auto form = uimodel::TrackPropertiesFormModel{ao::test::englishMessageCatalog(), 2};
    auto const field = rt::TrackField::Album;
    form.addField(field, true);
    form.loadFirstTrackField(field, std::string{"First"});

    SECTION("different albums")
    {
      REQUIRE(form.tryMergeTrackField(field, std::string{"Second"}));
    }

    SECTION("common album")
    {
      REQUIRE_FALSE(form.tryMergeTrackField(field, std::string{"First"}));
    }

    auto const row = projectTrackPropertyRow({.field = field, .label = "Album"}, form.rowView(field));
    REQUIRE(row.enabled);
    REQUIRE(synchronizeTrackPropertyInput(form, row, row.text, false));
    CHECK_FALSE(form.buildPatch().optAlbum);
    REQUIRE(synchronizeTrackPropertyInput(form, row, "Together", false));
    CHECK(form.buildPatch().optAlbum == "Together");
    REQUIRE(synchronizeTrackPropertyInput(form, row, "", false));
    CHECK_FALSE(form.buildPatch().optAlbum);
    REQUIRE(synchronizeTrackPropertyInput(form, row, "", true));
    CHECK(form.buildPatch().optAlbum == "");
    REQUIRE(synchronizeTrackPropertyInput(form, row, "", false));
    CHECK_FALSE(form.buildPatch().optAlbum);
  }

  TEST_CASE("TrackPropertiesAdapter - projects date rows like editable text metadata")
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

      CHECK(projected.controlKind == uimodel::TrackPropertiesFormEditorKind::Date);
      CHECK(projected.enabled);
      CHECK(projected.placeholder.empty());
    }

    SECTION("mixed date accepts a replacement without parsing the placeholder")
    {
      auto const projected = projectTrackPropertyRow(row,
                                                     uimodel::TrackPropertiesFormRowView{
                                                       .field = rt::TrackField::RecordingDate,
                                                       .text = "<Multiple Values>",
                                                       .mixed = true,
                                                       .editable = true,
                                                     });

      CHECK(projected.controlKind == uimodel::TrackPropertiesFormEditorKind::Date);
      CHECK(projected.enabled);
      CHECK(projected.text.empty());
      CHECK(projected.placeholder == "<Multiple Values>");
    }
  }

  TEST_CASE("TrackPropertiesAdapter - batch numeric zero does not implicitly clear a stored number")
  {
    auto form = uimodel::TrackPropertiesFormModel{ao::test::englishMessageCatalog(), 2};
    auto const field = rt::TrackField::Year;
    form.addField(field, true);
    form.loadFirstTrackField(field, std::uint16_t{1981});
    auto const row = projectTrackPropertyRow(
      {.field = field, .label = "Year", .editorKind = uimodel::TrackPropertiesFormEditorKind::Number},
      form.rowView(field));
    CHECK_FALSE(synchronizeTrackPropertyInput(form, row, "20x6", false));
    REQUIRE(synchronizeTrackPropertyInput(form, row, "2026", false));
    CHECK(form.buildPatch().optYear == 2026);
    REQUIRE(synchronizeTrackPropertyInput(form, row, " 0 ", false));
    CHECK_FALSE(form.buildPatch().optYear);
    REQUIRE(synchronizeTrackPropertyInput(form, row, row.text, true));
    CHECK(form.buildPatch().optYear == 0);

    auto single = uimodel::TrackPropertiesFormModel{ao::test::englishMessageCatalog(), 1};
    single.addField(field, true);
    single.loadFirstTrackField(field, std::uint16_t{1981});
    REQUIRE(synchronizeTrackPropertyInput(single, row, "", false));
    CHECK(single.buildPatch().optYear == 0);
  }

  TEST_CASE("TrackPropertiesAdapter - invalid input withdraws only its prior field edit")
  {
    auto form = uimodel::TrackPropertiesFormModel{ao::test::englishMessageCatalog(), 1};
    constexpr auto kYear = rt::TrackField::Year;
    constexpr auto kTitle = rt::TrackField::Title;
    form.addField(kYear, true);
    form.loadFirstTrackField(kYear, std::uint16_t{2003});
    form.addField(kTitle, true);
    form.loadFirstTrackField(kTitle, std::string{"Before"});
    auto const year = projectTrackPropertyRow(
      {.field = kYear, .label = "Year", .editorKind = uimodel::TrackPropertiesFormEditorKind::Number},
      form.rowView(kYear));
    auto const title = projectTrackPropertyRow({.field = kTitle, .label = "Title"}, form.rowView(kTitle));

    REQUIRE(synchronizeTrackPropertyInput(form, title, "After", false));
    REQUIRE(synchronizeTrackPropertyInput(form, year, "2010", false));
    REQUIRE(form.buildPatch().optYear == 2010);

    REQUIRE_FALSE(synchronizeTrackPropertyInput(form, year, "2010x", false));
    auto const invalid = form.buildPatch();
    CHECK_FALSE(invalid.optYear);
    CHECK(invalid.optTitle == "After");

    REQUIRE(synchronizeTrackPropertyInput(form, year, "2011", false));
    auto const corrected = form.buildPatch();
    CHECK(corrected.optYear == 2011);
    CHECK(corrected.optTitle == "After");
  }

  TEST_CASE("TrackPropertiesAdapter - presentation requires a nonempty selection")
  {
    CHECK_FALSE(canPresentTrackProperties({}));
    CHECK(canPresentTrackProperties(std::array{TrackId{7}}));
  }

  TEST_CASE("TrackPropertiesAdapter - projects authoring commit states for a current binding")
  {
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Applied, true) ==
          TrackPropertiesCommitState::Accepted);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::NoOp, true) == TrackPropertiesCommitState::Accepted);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Busy, true) == TrackPropertiesCommitState::Busy);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Stale, true) == TrackPropertiesCommitState::Stale);
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Unavailable, true) ==
          TrackPropertiesCommitState::Unavailable);
  }

  TEST_CASE("TrackPropertiesAdapter - Busy needs reload when the binding is no longer current")
  {
    CHECK(projectTrackPropertiesCommitState(rt::AuthoringStatus::Busy, false) == TrackPropertiesCommitState::Stale);
  }

  TEST_CASE("TrackPropertiesAdapter - blocked input defers Busy retry until the draft is submit ready")
  {
    using Feedback = TrackPropertiesRetryFeedback;
    using Draft = TrackPropertiesRetryDraft;
    auto const deferred = projectTrackPropertiesRetryFeedback(Feedback::Visible, true, Draft::Blocked);
    REQUIRE(deferred == Feedback::Deferred);

    SECTION("correcting invalid input restores retry for the remaining edit")
    {
      CHECK(projectTrackPropertiesRetryFeedback(deferred, true, Draft::SubmitReady) == Feedback::Visible);
    }

    SECTION("reverting the only edit retires retry even if a new edit follows")
    {
      auto const cleared = projectTrackPropertiesRetryFeedback(deferred, true, Draft::Unchanged);
      REQUIRE(cleared == Feedback::None);
      CHECK(projectTrackPropertiesRetryFeedback(cleared, true, Draft::SubmitReady) == Feedback::None);
    }

    SECTION("a noncurrent binding retires the deferred retry even if the draft is valid")
    {
      CHECK(projectTrackPropertiesRetryFeedback(deferred, false, Draft::SubmitReady) == Feedback::None);
    }
  }

  TEST_CASE("TrackPropertiesAdapter - projects tag and custom-key vocabulary")
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

  TEST_CASE("TrackPropertiesAdapter - explicit empty custom value replaces a mixed original")
  {
    CHECK(needsCustomMetadataValueUpdate(true, std::nullopt, ""));
    CHECK(needsCustomMetadataValueUpdate(true, std::nullopt, "Ambient"));
    CHECK_FALSE(needsCustomMetadataValueUpdate(true, std::optional<std::string>{""}, ""));
    CHECK_FALSE(needsCustomMetadataValueUpdate(true, std::optional<std::string>{"Ambient"}, "Ambient"));
    CHECK(needsCustomMetadataValueUpdate(true, std::optional<std::string>{"Ambient"}, ""));
    CHECK(needsCustomMetadataValueUpdate(false, std::nullopt, ""));
  }

  TEST_CASE("TrackPropertiesAdapter - category previews never expose scalar credit edits")
  {
    for (auto const field : {rt::TrackField::Conductor, rt::TrackField::Ensemble, rt::TrackField::Soloist})
    {
      auto const projected = projectTrackPropertyRow(
        {.field = field, .label = "Credit", .editorKind = uimodel::TrackPropertiesFormEditorKind::Text},
        {.field = field, .text = "First +2", .editable = true});
      CHECK(projected.controlKind == uimodel::TrackPropertiesFormEditorKind::ReadonlyText);
      CHECK_FALSE(projected.enabled);
      CHECK(projected.text == "First +2");
    }

    auto const composer =
      projectTrackPropertyRow({.field = rt::TrackField::Composer,
                               .label = "Composer",
                               .editorKind = uimodel::TrackPropertiesFormEditorKind::Text},
                              {.field = rt::TrackField::Composer, .text = "Bach", .editable = true});
    CHECK(composer.controlKind == uimodel::TrackPropertiesFormEditorKind::Text);
    CHECK(composer.enabled);
    CHECK(composer.text == "Bach");
  }

  TEST_CASE("TrackPropertiesAdapter - active credit child blocks tags and ordinary field submission")
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

  TEST_CASE("TrackPropertiesAdapter - accepting credits stages one typed scope with the ordinary draft")
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
