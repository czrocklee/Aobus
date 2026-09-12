// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/EventControllerTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/EventController.h"
#include "tui/HitRegions.h"
#include "tui/MouseBindings.h"
#include "tui/PlaybackPanel.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <chrono>
#include <cstdint>
#include <list>
#include <utility>

namespace ao::tui::test
{
  namespace
  {
    void paintPlayback(HitRegions& hit, rt::PlaybackTransportSnapshot const& state, std::int32_t const columns)
    {
      std::ignore = renderElement(playbackBar(ao::test::englishMessageCatalog(),
                                              {.playbackState = &state,
                                               .displayElapsed = state.elapsed,
                                               .soulButtonBox = &hit.soulButtonBox,
                                               .playbackModeBox = &hit.playbackModeBox,
                                               .metadataHitRegions = &hit.playbackMetadata,
                                               .terminalColumns = columns}),
                                  columns,
                                  1);
    }
  } // namespace

  TEST_CASE("EventController - painted playback geometry retains hover only while its target contains the pointer",
            "[tui][regression][playback-hover]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    auto state =
      rt::PlaybackTransportSnapshot{.elapsed = std::chrono::seconds{599}, .duration = std::chrono::minutes{20}};
    std::int32_t columns = 140;
    auto const nextColumns = columns;
    auto nextState = state;
    bool pointAtLeftEdge = false;
    bool targetMoves = true;

    SECTION("duration resolves")
    {
      state.duration = std::chrono::seconds{0};
      state.elapsed = std::chrono::seconds{0};
      nextState = state;
      nextState.duration = std::chrono::seconds{225};
      pointAtLeftEdge = true;
    }

    SECTION("elapsed gains a digit")
    {
      nextState.elapsed = std::chrono::seconds{600};
      targetMoves = false;
    }

    SECTION("terminal becomes narrower")
    {
      columns = 160;
    }

    paintPlayback(fixture.hitRegions, state, columns);
    auto const before = fixture.hitRegions.playbackModeBox;
    REQUIRE_FALSE(before.IsEmpty());
    auto const mouse = ftxui::Mouse{.button = ftxui::Mouse::Left,
                                    .motion = ftxui::Mouse::Moved,
                                    .x = pointAtLeftEdge ? before.x_min : before.x_max,
                                    .y = before.y_min};
    REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    REQUIRE(events.hoveredButton() == HoveredButton::PlaybackMode);

    auto mode = rt::PlaybackSuccessionSnapshot{};
    auto settingsBox = kEmptyMouseBox;
    auto actions = std::list<StatusActionHitRegion>{};
    auto renderStatus = [&]
    {
      return renderElement(
        statusBar(library.textCatalog(),
                  {.terminalColumns = columns,
                   .settingsButtonBox = &settingsBox,
                   .hoveredPlaybackMode = events.hoveredButton() == HoveredButton::PlaybackMode ? &mode : nullptr,
                   .actionHitRegions = &actions},
                  defaultKeymapPlan()),
        columns,
        1);
    };
    CHECK(renderStatus().text.contains("Click for"));
    REQUIRE(settingsBox.IsEmpty());

    columns = nextColumns;
    paintPlayback(fixture.hitRegions, nextState, columns);

    if (!targetMoves)
    {
      CHECK(contains(fixture.hitRegions.playbackModeBox, mouse.x, mouse.y));
      CHECK_FALSE(events.tryRetireHover());
      CHECK(events.hoveredButton() == HoveredButton::PlaybackMode);
      CHECK(renderStatus().text.contains("Click for"));
      return;
    }

    REQUIRE_FALSE(contains(fixture.hitRegions.playbackModeBox, mouse.x, mouse.y));
    CHECK(events.tryRetireHover());
    CHECK(events.hoveredButton() == HoveredButton::None);
    CHECK_FALSE(events.tryRetireHover());
    auto const status = renderStatus();
    CHECK_FALSE(status.text.contains("Click for"));
    CHECK(status.text.contains("Settings"));
    CHECK_FALSE(settingsBox.IsEmpty());
    CHECK_FALSE(actions.empty());
  }

  TEST_CASE("EventController - height-only workspace resize preserves valid top-bar hover",
            "[tui][regression][playback-hover]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.hitRegions.trackTableBox = {.x_min = 1, .x_max = 138, .y_min = 2, .y_max = 20};
    events.syncWorkspaceGeometry();
    paintPlayback(fixture.hitRegions, {}, 140);
    auto const box = fixture.hitRegions.playbackModeBox;
    REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = box.x_min, .y = box.y_min})));
    fixture.hitRegions.trackTableBox.y_max = 18;
    paintPlayback(fixture.hitRegions, {}, 140);
    events.syncWorkspaceGeometry();
    CHECK_FALSE(events.tryRetireHover());
    CHECK(events.hoveredButton() == HoveredButton::PlaybackMode);
  }

  TEST_CASE("EventController - metadata updates retire hover on a vanished link", "[tui][regression][playback-hover]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    auto state = rt::PlaybackTransportSnapshot{
      .nowPlaying = {.trackId = TrackId{42}, .title = "Title", .artist = "Artist"},
    };
    paintPlayback(fixture.hitRegions, state, 140);
    auto const box = fixture.hitRegions.playbackMetadata.artist;
    REQUIRE_FALSE(box.IsEmpty());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = box.x_min, .y = box.y_min})));
    REQUIRE(events.hoveredButton() == HoveredButton::PlaybackArtist);
    state.nowPlaying.artist.clear();
    paintPlayback(fixture.hitRegions, state, 140);
    REQUIRE(fixture.hitRegions.playbackMetadata.artist.IsEmpty());
    CHECK(events.tryRetireHover());
    CHECK(events.hoveredButton() == HoveredButton::None);
  }

  TEST_CASE("EventController - painted hover refresh never resurrects a cancelled interaction",
            "[tui][regression][playback-hover]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    paintPlayback(fixture.hitRegions, {}, 140);
    auto const box = fixture.hitRegions.playbackModeBox;
    REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = box.x_min, .y = box.y_min})));
    events.cancelTransientInteractions();
    paintPlayback(fixture.hitRegions, {}, 140);
    CHECK_FALSE(events.tryRetireHover());
    CHECK(events.hoveredButton() == HoveredButton::None);
  }

  TEST_CASE("EventController - opening an overlay retires quality hover without another redraw",
            "[tui][regression][playback-hover]")
  {
    auto fixture = EventControllerFixture{};
    fixture.preferences.qualityHover = true;
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    paintPlayback(fixture.hitRegions, {}, 140);
    auto const box = fixture.hitRegions.soulButtonBox;
    REQUIRE_FALSE(box.IsEmpty());
    REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = box.x_min, .y = box.y_min})));
    REQUIRE(events.isQualityHoverVisible());

    REQUIRE(events.tryHandleEvent(ftxui::Event::Character('?')));

    REQUIRE(fixture.shell.overlay() == Overlay::Help);
    CHECK_FALSE(events.isQualityHoverVisible());
    CHECK(events.hoveredButton() == HoveredButton::None);
    CHECK_FALSE(events.tryRetireHover());
  }
} // namespace ao::tui::test
