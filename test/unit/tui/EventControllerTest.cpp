// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/EventController.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "tui/LibraryController.h"
#include "tui/ShellModel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <chrono>
#include <string_view>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    struct EventControllerFixture final
    {
      ao::test::TempDir tempDir{};
      rt::AppRuntime runtime{rt::test::makeRuntime(tempDir)};
      ftxui::ScreenInteractive screen{ftxui::ScreenInteractive::FixedSize(80, 24)};
      ShellModel shell{};

      EventControllerFixture()
      {
        library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "First"});
        library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "Second"});
      }

      LibraryController makeLibrary() { return LibraryController{runtime}; }
    };

    void enterCommand(EventController& controller, std::string_view text)
    {
      CHECK(controller.handleEvent(ftxui::Event::Character("/")));

      for (char const ch : text)
      {
        CHECK(controller.handleEvent(ftxui::Event::Character(std::string{ch})));
      }

      CHECK(controller.handleEvent(ftxui::Event::Return));
    }
  } // namespace

  TEST_CASE("EventController - command input is modal for navigation keys", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));

    CHECK(fixture.shell.commandActive());
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - command input escape cancels the draft", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character(":")));
    CHECK(controller.handleEvent(ftxui::Event::Character("h")));
    CHECK(fixture.shell.commandActive());
    CHECK(fixture.shell.commandDraft() == "h");

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.commandActive());
    CHECK(fixture.shell.commandDraft().empty());
    CHECK(controller.statusMessage() == "Command cancelled");
  }

  TEST_CASE("EventController - detail shortcut toggles the detail overlay", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - overlay shortcuts publish visible shell state", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("l")));
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(controller.statusMessage() == "Lists");

    CHECK(controller.handleEvent(ftxui::Event::Character("?")));
    CHECK(fixture.shell.overlay() == Overlay::Help);
    CHECK(controller.statusMessage() == "Help");

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Overlay closed");

    CHECK(controller.handleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::QualityPanel);
    CHECK(controller.statusMessage() == "Audio quality");

    CHECK(controller.handleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Quality closed");
  }

  TEST_CASE("EventController - navigation shortcuts move the focused selection", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    REQUIRE(library.tracks().size() == 2);
    CHECK(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    CHECK(library.selectedTrack() == 1);

    CHECK(controller.handleEvent(ftxui::Event::ArrowUp));
    CHECK(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::End));
    CHECK(library.selectedTrack() == 1);

    CHECK(controller.handleEvent(ftxui::Event::Home));
    CHECK(library.selectedTrack() == 0);

    fixture.shell.openOverlay(Overlay::ListChooser);
    CHECK(controller.handleEvent(ftxui::Event::PageDown));
    CHECK(library.selectedList() == 0);
    CHECK(controller.handleEvent(ftxui::Event::PageUp));
    CHECK(library.selectedList() == 0);
  }

  TEST_CASE("EventController - commands apply filters and clear them", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    enterCommand(controller, "First");
    CHECK_FALSE(fixture.shell.commandActive());
    CHECK(library.filterDraft() == "First");
    CHECK(library.tracks().size() == 1);
    REQUIRE(library.selectedTrackView().track != nullptr);
    CHECK(library.selectedTrackView().track->row.title == "First");
    CHECK(controller.statusMessage() == "Quick filter matched 1 tracks");

    enterCommand(controller, "clear");
    CHECK(library.filterDraft().empty());
    CHECK(library.tracks().size() == 2);
    CHECK(controller.statusMessage() == "Filter cleared");
  }

  TEST_CASE("EventController - named commands route to shell playback and library actions", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    enterCommand(controller, "lists");
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(controller.statusMessage() == "Lists");

    enterCommand(controller, "detail");
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(controller.statusMessage() == "Detail panel");

    enterCommand(controller, "quality");
    CHECK(fixture.shell.overlay() == Overlay::QualityPanel);
    CHECK(controller.statusMessage() == "Audio quality");

    enterCommand(controller, "close");
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Overlay closed");

    enterCommand(controller, "help");
    CHECK(fixture.shell.overlay() == Overlay::Help);
    CHECK(controller.statusMessage() == "Help");

    enterCommand(controller, "current");
    CHECK(controller.statusMessage() == "No current track");

    enterCommand(controller, "view albums");
    CHECK(controller.statusMessage() == "View: albums");
    CHECK(fixture.runtime.views().trackListState(library.activeViewId()).presentation.id == "albums");

    enterCommand(controller, "reload");
    CHECK(controller.statusMessage() == "Reloaded 2 tracks");

    enterCommand(controller, "play");
    CHECK(controller.statusMessage() == "Playback requested");

    enterCommand(controller, "toggle");
    CHECK(controller.statusMessage() == "Playback toggled");

    enterCommand(controller, "stop");
    CHECK(controller.statusMessage() == "Stopped");
  }

  TEST_CASE("EventController - list chooser return opens the selected list", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    fixture.shell.openOverlay(Overlay::ListChooser);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(library.currentListTitle() == "All Tracks");
    CHECK(controller.statusMessage() == "Opened All Tracks");
  }

  TEST_CASE("EventController - playback shortcuts update status and controls", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};
    auto seekEvents = std::vector<std::chrono::milliseconds>{};
    auto sub = fixture.runtime.playback().onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                       { seekEvents.push_back(event.elapsed); });

    fixture.runtime.playback().setVolume(0.50F);

    CHECK(controller.handleEvent(ftxui::Event::Character("p")));
    CHECK(controller.statusMessage() == "Playback requested");

    CHECK(controller.handleEvent(ftxui::Event::Character("[")));
    CHECK(controller.handleEvent(ftxui::Event::Character("]")));
    REQUIRE(seekEvents.size() == 2);
    CHECK(seekEvents[0] == std::chrono::milliseconds{0});
    CHECK(seekEvents[1] == std::chrono::seconds{5});

    CHECK(controller.handleEvent(ftxui::Event::Character("-")));
    CHECK(fixture.runtime.playback().state().volume < 0.50F);

    CHECK(controller.handleEvent(ftxui::Event::Character("+")));
    CHECK(fixture.runtime.playback().state().volume > 0.49F);

    CHECK(controller.handleEvent(ftxui::Event::Character("s")));
    CHECK(controller.statusMessage() == "Stopped");
  }

  TEST_CASE("EventController - current track shortcut reveals playback selection", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    REQUIRE(library.selectedTrack() == 1);
    CHECK(controller.handleEvent(ftxui::Event::Character("p")));
    REQUIRE(controller.statusMessage() == "Playback requested");

    CHECK(controller.handleEvent(ftxui::Event::Home));
    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::CtrlL));
    CHECK(library.selectedTrack() == 1);
    CHECK(controller.statusMessage() == "Revealed Second");
  }
} // namespace ao::tui::test
