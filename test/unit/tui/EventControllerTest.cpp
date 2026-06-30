// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/EventController.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "tui/LibraryController.h"
#include "tui/ShellModel.h"
#include <ao/rt/AppRuntime.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

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
} // namespace ao::tui::test
