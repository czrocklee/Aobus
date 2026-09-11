// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/GoToMenu.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/EventControllerTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/Command.h"
#include "tui/EventController.h"
#include "tui/HitRegions.h"
#include "tui/Keymap.h"
#include "tui/LibraryController.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/pixel.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ao::tui::test
{
  namespace
  {
    void press(EventController& events, char const key)
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character(key)));
    }
  } // namespace

  TEST_CASE("GoToMenu - prefix navigation preserves playback and traverses view history", "[tui][unit][goto]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.addReadyAudioProvider();
    auto& playback = fixture.runtimePtr->playback();
    auto const target = library.tracks().front().id;
    REQUIRE(playback.commands().startFromView(library.activeViewId(), target));
    REQUIRE(fixture.tryWaitForPlayback(target));
    library.setFilterDraft("Second");
    REQUIRE(library.applyFilter());
    auto const previous = library.activeViewId();
    press(events, 'g');
    CHECK(fixture.shell.overlay() == Overlay::GoTo);
    CHECK(library.activeViewId() == previous);

    SECTION("Track suffix reveals the playing subject")
    {
      press(events, 't');
      fixture.executor->drain();
      REQUIRE(library.selectedTrackView().track != nullptr);
      CHECK(library.selectedTrackView().track->id == target);
    }

    SECTION("Artist suffix opens exact artist results")
    {
      press(events, 'a');
      CHECK(library.filterDraft() == "$artist = \"Artist\"");
    }

    SECTION("Album suffix opens album groups")
    {
      press(events, 'b');
      CHECK(library.activePresentationId() == "albums");
    }

    SECTION("Menu selection activates with Enter")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::ArrowDown));
      REQUIRE(events.tryHandleEvent(ftxui::Event::Return));
      CHECK(library.filterDraft() == "$artist = \"Artist\"");
    }

    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(events.goToMenuState().nowPlaying.trackId == target);
    auto const destination = library.activeViewId();
    press(events, 'g');
    press(events, '[');
    CHECK(library.activeViewId() == previous);
    press(events, 'g');
    press(events, ']');
    CHECK(library.activeViewId() == destination);
  }

  TEST_CASE("GoToMenu - unavailable actions and cancellation never leak into workspace keys", "[tui][unit][goto]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    library.toggleVisualSelection();
    auto const previous = library.activeViewId();
    press(events, 'g');

    SECTION("Missing playback leaves the menu open")
    {
      press(events, 'a');
      CHECK(fixture.shell.overlay() == Overlay::GoTo);
      REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    }

    SECTION("Unavailable forward history leaves the menu open")
    {
      press(events, ']');
      CHECK(fixture.shell.overlay() == Overlay::GoTo);
      REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    }

    SECTION("An unrelated quit key only cancels the prefix")
    {
      press(events, 'Q');
      CHECK(fixture.exitRequestCount == 0);
    }

    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(library.isVisualSelectionActive());
    CHECK(library.activeViewId() == previous);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(library.isVisualSelectionActive());
  }

  TEST_CASE("GoToMenu - text input retains printable prefix keys", "[tui][unit][goto]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.beginInput(ShellInputMode::QuickFilter, "");
    press(events, 'g');
    press(events, 'a');
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.shell.inputDraft() == "ga");
  }

  TEST_CASE("GoToMenu - configurable prefix and direct actions share executable hints", "[tui][unit][goto]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({{"tui.navigation.openGoTo", {"Z"}}});
    auto const plan = KeymapPlan{model};
    CHECK(commandShortcut(plan, CommandAction::OpenGoTo) == "z");
    CHECK(commandShortcut(plan, CommandAction::OpenCurrentArtist) == "z a");
    CHECK(commandShortcut(plan, CommandAction::OpenCurrentAlbum) == "z b");
    CHECK(commandShortcut(plan, CommandAction::Back) == "z [");
    CHECK(commandShortcut(plan, CommandAction::Forward) == "z ]");
    CHECK(commandShortcut(plan, CommandAction::RevealCurrentTrack) == "c");
    CHECK_FALSE(plan.actionFor(ftxui::Event::Character('g')));
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library, plan);
    fixture.shell.focusNavigation();
    press(events, 'z');
    CHECK(fixture.shell.overlay() == Overlay::GoTo);
    model.applyOverrides({{"tui.navigation.openGoTo", {}}, {"tui.navigation.currentArtist", {"F4"}}});
    auto const directPlan = KeymapPlan{model};
    CHECK(commandShortcut(directPlan, CommandAction::OpenCurrentArtist) == "F4");
    CHECK(commandShortcut(directPlan, CommandAction::OpenCurrentAlbum).empty());
    model.applyOverrides({{"tui.shell.toggleAudioQuality", {"G"}}});
    auto const legacyPlan = KeymapPlan{model};
    CHECK(legacyPlan.actionFor(ftxui::Event::Character('g')) == KeyAction::ToggleAudioPipeline);
    CHECK(commandShortcut(legacyPlan, CommandAction::OpenCurrentArtist).empty());
  }

  TEST_CASE("GoToMenu - status hints follow state and retain suffixes at narrow widths", "[tui][unit][goto]")
  {
    for (auto const* locale : {"en", "zh-Hans", "ja", "de", "fr", "es", "zh-Hant"})
    {
      auto const catalog = ao::test::messageCatalog(locale);

      for (std::int32_t const columns : {140, 80, 48, 24})
      {
        CAPTURE(locale, columns);
        auto shell = ShellInteractionModel{};
        auto hit = GoToMenuHitRegions{};
        shell.openOverlay(Overlay::GoTo);
        auto const rendered = renderElement(
          statusBar(
            catalog, {.terminalColumns = columns, .shell = &shell, .goToHitRegions = &hit}, defaultKeymapPlan()),
          columns,
          1);
        INFO(rendered.text);
        REQUIRE(hit.rows.size() == 5);

        auto const suffixes = std::string{"tab[]"};
        std::size_t suffixIndex = 0;

        for (auto const& row : hit.rows)
        {
          REQUIRE_FALSE(row.box.IsEmpty());
          CHECK(rendered.screen.PixelAt(row.box.x_min, row.box.y_min).character ==
                std::string(1, suffixes[suffixIndex++]));
          CHECK(row.box.x_min >= 0);
          CHECK(row.box.x_max < columns);
          CHECK_FALSE(canActivateGoTo(hit.state, row.action));
        }

        CHECK(rendered.text.contains("Esc"));
        REQUIRE_FALSE(hit.cancelBox.IsEmpty());
        CHECK(rendered.screen.PixelAt(hit.cancelBox.x_min, hit.cancelBox.y_min).character == "E");
        CHECK(rendered.screen.PixelAt(hit.cancelBox.x_min + 1, hit.cancelBox.y_min).character == "s");
        CHECK(rendered.screen.PixelAt(hit.cancelBox.x_min + 2, hit.cancelBox.y_min).character == "c");
        shell.closeOverlay();
        renderElement(
          statusBar(
            catalog, {.terminalColumns = columns, .shell = &shell, .goToHitRegions = &hit}, defaultKeymapPlan()),
          columns,
          1);
        CHECK(hit.rows.empty());
        CHECK(hit.cancelBox.IsEmpty());
      }
    }
  }

  TEST_CASE("GoToMenu - footer targets navigate or cancel and reject stale state", "[tui][unit][goto]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    REQUIRE(library.navigateToArtist("Artist"));
    auto const previous = library.activeViewId();
    press(events, 'g');
    // A real popup bounds the workspace; footer targets remain actionable outside it.
    fixture.hitRegions.overlayPanel.box = {.x_min = 10, .x_max = 65, .y_min = 5, .y_max = 18};
    auto& hit = fixture.hitRegions.goToStatus;
    auto const rendered =
      renderElement(goToHintBar(ao::test::englishMessageCatalog(), events.goToMenuState(), 80, &hit), 80, 1);
    INFO(rendered.text);
    auto const back = std::ranges::find(hit.rows, CommandAction::Back, &GoToMenuRowHitRegion::action);
    REQUIRE(back != hit.rows.end());

    SECTION("Back footer chip navigates")
    {
      REQUIRE(events.tryHandleEvent(clickBox(back->box)));
      CHECK(library.activeViewId() != previous);
      CHECK(fixture.shell.overlay() == Overlay::None);
    }

    SECTION("Cancel footer chip closes without navigating")
    {
      REQUIRE(events.tryHandleEvent(clickBox(hit.cancelBox)));
      CHECK(library.activeViewId() == previous);
      CHECK(fixture.shell.overlay() == Overlay::None);
    }

    SECTION("Changed rendered state cannot activate a stale chip")
    {
      hit.state.canGoBack = false;
      REQUIRE(events.tryHandleEvent(clickBox(back->box)));
      CHECK(library.activeViewId() == previous);
      CHECK(fixture.shell.overlay() == Overlay::GoTo);
    }
  }

  TEST_CASE("GoToMenu - painted menu targets share navigation with footer and keyboard", "[tui][unit][goto]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    REQUIRE(library.navigateToArtist("Artist"));
    auto const previous = library.activeViewId();
    press(events, 'g');
    auto const state = events.goToMenuState();
    auto& menuHit = fixture.hitRegions.goToMenu;
    auto& footerHit = fixture.hitRegions.goToStatus;
    auto const footer = renderElement(goToHintBar(ao::test::englishMessageCatalog(), state, 80, &footerHit), 80, 1);
    auto const menu = renderElement(goToMenu(ao::test::englishMessageCatalog(), state, 3, 60, &menuHit), 60, 16);
    INFO(menu.text);
    CHECK(menu.text.contains("reveal current track"));
    REQUIRE(menuHit.rows.size() == 5);
    REQUIRE(footerHit.rows.size() == 5);

    for (auto const& row : menuHit.rows)
    {
      REQUIRE_FALSE(row.box.IsEmpty());
      CHECK(menu.screen.PixelAt(row.box.x_min, row.box.y_min).dim == !canActivateGoTo(state, row.action));
    }

    auto const back = std::ranges::find(menuHit.rows, CommandAction::Back, &GoToMenuRowHitRegion::action);
    REQUIRE(back != menuHit.rows.end());
    fixture.hitRegions.overlayPanel.box = {.x_min = 0, .x_max = 59, .y_min = 0, .y_max = 15};
    REQUIRE(events.tryHandleEvent(clickBox(back->box)));
    CHECK(library.activeViewId() != previous);
    CHECK(fixture.shell.overlay() == Overlay::None);
  }
} // namespace ao::tui::test
