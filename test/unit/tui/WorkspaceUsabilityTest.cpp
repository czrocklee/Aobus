// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/CommandPalettePanel.h"
#include "tui/Keymap.h"
#include "tui/PlaybackPanel.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include "tui/TrackTable.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <list>
#include <string>

namespace ao::tui::test
{
  TEST_CASE("StatusBar - narrow filtered workspaces keep the condition and recovery controls",
            "[tui][regression][usability]")
  {
    for (auto const* locale : {"en", "de", "zh-Hans"})
    {
      INFO(locale);
      auto const catalog = ao::test::messageCatalog(locale);
      auto activity = uimodel::ActivityStatusViewState{
        .compact = {.kind = uimodel::ActivityStatusKind::Info, .text = std::string(200, 'i')}};
      auto activityBox = ftxui::Box{.x_max = 10, .y_max = 0};
      auto actions = std::list<StatusActionHitRegion>{};
      auto const rendered = renderElement(statusBar(catalog,
                                                    {.activityStatus = &activity,
                                                     .terminalColumns = 48,
                                                     .filterDraft = "Alpha",
                                                     .activityStatusBox = &activityBox,
                                                     .actionHitRegions = &actions},
                                                    defaultKeymapPlan()),
                                          48,
                                          1);
      CHECK(rendered.text.contains("/ Alpha"));
      CHECK(rendered.text.contains(i18n::requiredText(catalog, i18n::MessageId::TuiStatusClear)));
      CHECK(rendered.text.contains(i18n::requiredText(catalog, i18n::MessageId::TuiSettingsTitle)));
      CHECK(activityBox.IsEmpty());

      for (auto const& action : actions)
      {
        CHECK(!action.box.IsEmpty());
        CHECK(action.box.x_min >= 0);
        CHECK(action.box.x_max < 48);
      }
    }
  }

  TEST_CASE("StatusBar - narrow warning remains discoverable without displacing filter recovery",
            "[tui][regression][usability]")
  {
    auto activity = uimodel::ActivityStatusViewState{
      .compact = {.kind = uimodel::ActivityStatusKind::Warning, .text = "An actionable warning"}};
    auto activityBox = ftxui::Box{};
    auto const rendered = renderElement(statusBar(ao::test::englishMessageCatalog(),
                                                  {.activityStatus = &activity,
                                                   .terminalColumns = 48,
                                                   .filterDraft = std::string(200, 'f'),
                                                   .activityStatusBox = &activityBox},
                                                  defaultKeymapPlan()),
                                        48,
                                        1);
    CHECK(rendered.text.contains("!"));
    CHECK(rendered.text.contains("…"));
    CHECK(rendered.text.contains("C clear"));
    CHECK(rendered.text.contains("Settings"));
    CHECK(!activityBox.IsEmpty());
    CHECK(activityBox.x_max < 48);
  }

  TEST_CASE("StatusBar - invalid filter text is visibly marked for correction", "[tui][unit][usability]")
  {
    auto const rendered =
      renderText(statusBar(ao::test::englishMessageCatalog(),
                           {.terminalColumns = 80, .filterDraft = "$artist =", .filterInvalid = true},
                           defaultKeymapPlan()),
                 80);
    CHECK(rendered.contains("/ ! $artist ="));
    CHECK(rendered.contains("C clear"));
  }

  TEST_CASE("QuickFilter - footer explains literal input and untouched filter transitions", "[tui][unit][usability]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::QuickFilter);
    auto rendered =
      renderText(quickFilterCompletionPanel(ao::test::englishMessageCatalog(), shell, defaultKeymapPlan(), 48), 48);
    CHECK(rendered.contains("Enter clear filter"));
    CHECK(rendered.contains("Esc keep filter"));
    shell.insertInputText("unlisted");
    rendered =
      renderText(quickFilterCompletionPanel(ao::test::englishMessageCatalog(), shell, defaultKeymapPlan(), 48), 48);
    CHECK(rendered.contains("No suggestions."));
    CHECK(rendered.contains("Enter apply text"));
    CHECK(rendered.contains("Esc keep text"));
    CHECK_FALSE(rendered.contains("Tab complete"));
  }

  TEST_CASE("PlaybackBar - narrow terminals retain title time and the complete volume value",
            "[tui][regression][usability]")
  {
    for (auto const* locale : {"en", "de", "zh-Hans"})
    {
      INFO(locale);
      auto state = rt::PlaybackTransportSnapshot{};
      state.nowPlaying.title = "Sample song";
      state.volume.level = 1.0F;
      auto volumeBox = ftxui::Box{};
      auto railBox = ftxui::Box{};
      auto const rendered = renderElement(
        playbackBar(ao::test::messageCatalog(locale),
                    {.playbackState = &state, .seekRailBox = &railBox, .volumeBox = &volumeBox, .terminalColumns = 48}),
        48,
        1);
      CHECK(rendered.text.contains("Sample"));
      CHECK(rendered.text.contains("0:00"));
      CHECK(rendered.text.contains("100%"));
      CHECK(!railBox.IsEmpty());
      CHECK(!volumeBox.IsEmpty());
      CHECK(railBox.x_max < volumeBox.x_min);
      CHECK(volumeBox.x_max < 48);
    }
  }

  TEST_CASE("TrackTable - narrow empty-state guidance wraps instead of losing the recovery instruction",
            "[tui][regression][usability]")
  {
    auto const rendered = renderElement(
      trackTableView(ao::test::englishMessageCatalog(),
                     {},
                     0,
                     kInvalidTrackId,
                     rt::defaultTrackPresentationSpec(),
                     {.availableColumns = 36, .emptyText = "No tracks match this filter. Change or clear the filter."}),
      36,
      8);
    CHECK(rendered.text.contains("No tracks match this filter."));
    CHECK(rendered.text.contains("clear the filter."));
    CHECK_FALSE(rendered.text.contains("aobus init"));
  }

  TEST_CASE("StatusBar - visual range explains Escape while the detail sidebar remains visible",
            "[tui][unit][usability]")
  {
    auto shell = ShellInteractionModel{};
    shell.toggleDetail();
    auto const rendered = renderText(statusBar(ao::test::englishMessageCatalog(),
                                               {.terminalColumns = 48, .visualSelectionActive = true, .shell = &shell},
                                               defaultKeymapPlan()),
                                     48);
    CHECK(rendered.contains("VISUAL"));
    CHECK(rendered.contains("v keep marks"));
    CHECK(rendered.contains("Esc cancel range"));
    CHECK_FALSE(rendered.contains("Esc close"));
  }

  TEST_CASE("QuickFilter - narrow German guidance preserves the complete no-suggestion explanation",
            "[tui][regression][usability]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::QuickFilter);
    auto const rows = quickFilterPanelRows(shell, false, 24);
    auto const rendered = renderElement(
      quickFilterCompletionPanel(ao::test::messageCatalog("de"), shell, defaultKeymapPlan(), 48), 48, rows);
    CHECK(rendered.text.contains("Keine Vorschläge."));
    CHECK(rendered.text.contains("nutzbar."));
    CHECK(rendered.text.contains("Enter Filter löschen"));
    CHECK(rendered.text.contains("Esc Filter behalten"));
  }

  TEST_CASE("PlaybackBar - missing title metadata does not masquerade as absent playback",
            "[tui][regression][usability]")
  {
    auto state = rt::PlaybackTransportSnapshot{};
    state.nowPlaying.trackId = TrackId{42};
    auto const rendered =
      renderText(playbackBar(ao::test::englishMessageCatalog(), {.playbackState = &state, .terminalColumns = 80}), 80);
    CHECK(rendered.contains("Track 42"));
    CHECK_FALSE(rendered.contains("No active track"));
  }

  TEST_CASE("StatusBar - empty track selections retire play action hints and mouse targets",
            "[tui][regression][usability]")
  {
    auto actions = std::list<StatusActionHitRegion>{};

    for (bool const hasSelection : {true, false})
    {
      auto const rendered = renderElement(
        statusBar(ao::test::englishMessageCatalog(),
                  {.terminalColumns = 200, .hasTrackSelection = hasSelection, .actionHitRegions = &actions},
                  defaultKeymapPlan()),
        200,
        1);
      CHECK(std::ranges::any_of(actions,
                                [](StatusActionHitRegion const& region)
                                { return region.action == KeyAction::PlaySelection && !region.box.IsEmpty(); }) ==
            hasSelection);
    }
  }
} // namespace ao::tui::test
