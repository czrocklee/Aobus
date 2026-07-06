// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/EventController.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "tui/LibraryController.h"
#include "tui/OutputDeviceController.h"
#include "tui/PlaybackPanel.h"
#include "tui/Render.h"
#include "tui/ShellModel.h"
#include "tui/TrackTable.h"
#include <ao/audio/Backend.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/box.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
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
        auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
        library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "First", .uri = fixturePath});
        library::test::addTrack(
          runtime.musicLibrary(), library::test::TrackSpec{.title = "Second", .uri = fixturePath});
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

  TEST_CASE("EventController - tab applies command completion without leaving command mode", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime.playback(),
      EventControllerBindings{
        .commandCompletionCallback = [](std::string_view const draft) -> std::optional<rt::CompletionResult>
        {
          if (draft != "de")
          {
            return std::nullopt;
          }

          return rt::CompletionResult{
            .replaceBegin = 0,
            .replaceEnd = 2,
            .items = {rt::CompletionItem{.displayText = "/detail", .insertText = "detail", .detail = "track detail"}},
          };
        }}};

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(controller.handleEvent(ftxui::Event::Character("e")));
    REQUIRE(fixture.shell.commandCompletion());

    CHECK(controller.handleEvent(ftxui::Event::Tab));
    CHECK(fixture.shell.commandActive());
    CHECK(fixture.shell.commandDraft() == "detail");
    CHECK_FALSE(fixture.shell.commandCompletion());

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.shell.commandActive());
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
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

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

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

  TEST_CASE("EventController - output shortcut toggles the output overlay", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto outputDevices = OutputDeviceController{fixture.runtime.playback()};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.outputDevices = &outputDevices}};

    CHECK(controller.handleEvent(ftxui::Event::Character("o")));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);
    CHECK(controller.statusMessage() == "Output devices");

    CHECK(controller.handleEvent(ftxui::Event::Character("o")));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Output devices closed");
  }

  TEST_CASE("EventController - modal overlays swallow workspace shortcuts", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.handleEvent(ftxui::Event::Character("a")));
    REQUIRE(fixture.shell.overlay() == Overlay::QualityPanel);
    REQUIRE(controller.statusMessage() == "Audio quality");

    auto const workspaceEvents = std::vector{
      ftxui::Event::PageDown,
      ftxui::Event::End,
      ftxui::Event::Return,
      ftxui::Event::Character("p"),
      ftxui::Event::Character(" "),
      ftxui::Event::Character("s"),
      ftxui::Event::Character("["),
      ftxui::Event::Character("]"),
      ftxui::Event::Character("+"),
      ftxui::Event::Character("-"),
      ftxui::Event::Character("="),
      ftxui::Event::Character("c"),
      ftxui::Event::Character("r"),
      ftxui::Event::Character("{"),
      ftxui::Event::Character("}"),
      ftxui::Event::CtrlL,
    };

    for (auto const& event : workspaceEvents)
    {
      CHECK(controller.handleEvent(event));
      CHECK(library.selectedTrack() == 0);
      CHECK(controller.statusMessage() == "Audio quality");
    }

    CHECK(controller.handleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Quality closed");
  }

  TEST_CASE("EventController - non-list modal overlays do not page the track table", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    REQUIRE(library.selectedTrack() == 0);

    fixture.shell.openOverlay(Overlay::OutputDevices);
    CHECK(controller.handleEvent(ftxui::Event::PageDown));
    CHECK(controller.handleEvent(ftxui::Event::Home));
    CHECK(controller.handleEvent(ftxui::Event::End));
    CHECK(library.selectedTrack() == 0);

    fixture.shell.openOverlay(Overlay::Help);
    CHECK(controller.handleEvent(ftxui::Event::PageDown));
    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - presentation shortcut toggles the views overlay closed", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Views closed");
  }

  TEST_CASE("EventController - presentation shortcut selects track views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);
    CHECK(controller.statusMessage() == "Views");

    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    CHECK(library.selectedPresentation() == 2);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "View: albums");
    CHECK(fixture.runtime.views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - presentation navigation keys move within views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(controller.handleEvent(ftxui::Event::End));
    CHECK(library.selectedPresentation() == static_cast<std::int32_t>(library.presentationItems().size()) - 1);

    CHECK(controller.handleEvent(ftxui::Event::Home));
    CHECK(library.selectedPresentation() == 0);

    CHECK(controller.handleEvent(ftxui::Event::PageDown));
    CHECK(library.selectedPresentation() == 10);

    CHECK(controller.handleEvent(ftxui::Event::PageUp));
    CHECK(library.selectedPresentation() == 0);

    CHECK(controller.handleEvent(ftxui::Event::ArrowUp));
    CHECK(library.selectedPresentation() == 0);
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
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "detail");
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(controller.statusMessage() == "Detail panel");
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "quality");
    CHECK(fixture.shell.overlay() == Overlay::QualityPanel);
    CHECK(controller.statusMessage() == "Audio quality");
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "views");
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);
    CHECK(controller.statusMessage() == "Views");
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "close");
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Overlay closed");

    enterCommand(controller, "help");
    CHECK(fixture.shell.overlay() == Overlay::Help);
    CHECK(controller.statusMessage() == "Help");
    CHECK(controller.handleEvent(ftxui::Event::Escape));

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

  TEST_CASE("EventController - output commands and mouse clicks select devices", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto outputDevices = OutputDeviceController{fixture.runtime.playback()};
    auto outputButtonBox = ftxui::Box{.x_min = 4, .x_max = 9, .y_min = 0, .y_max = 0};
    auto rowBoxes = std::vector{
      OutputDeviceRowBox{.rowIndex = 1, .box = ftxui::Box{.x_min = 2, .x_max = 30, .y_min = 3, .y_max = 3}}};
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime.playback(),
      EventControllerBindings{
        .outputDevices = &outputDevices, .outputDeviceButtonBox = &outputButtonBox, .outputDeviceRowBoxes = &rowBoxes}};

    auto clickBadge = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 6, .y = 0};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickBadge)));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    auto clickOrigin = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 0, .y = 0};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", clickOrigin)));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 3};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Output: Test Device");
    CHECK(fixture.runtime.playback().state().output.selectedDevice.backendId == audio::BackendId{"test_backend"});

    enterCommand(controller, "output");
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);
    CHECK(controller.statusMessage() == "Output devices");

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "Output: Test Device");
  }

  TEST_CASE("EventController - presentation mouse clicks open and select views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto presentationButtonBox = ftxui::Box{.x_min = 20, .x_max = 29, .y_min = 23, .y_max = 23};
    auto rowBoxes = std::vector{
      PresentationRowBox{.rowIndex = 2, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime.playback(),
      EventControllerBindings{.presentationButtonBox = &presentationButtonBox, .presentationRowBoxes = &rowBoxes}};

    auto clickView = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 24, .y = 23};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickView)));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 12};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.statusMessage() == "View: albums");
    CHECK(fixture.runtime.views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - mouse drag resizes track columns in session state", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto resizeHandles =
      std::vector{TrackColumnResizeHandle{.field = rt::TrackField::Title,
                                          .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                                          .columns = 20}};
    auto widthOverrides = std::vector<TrackColumnWidthOverride>{};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.trackColumnResizeHandles = &resizeHandles,
                                                              .trackColumnWidthOverrides = &widthOverrides}};

    auto pressEdge = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressEdge)));
    CHECK(controller.statusMessage() == "Resizing column");

    auto moveRight = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 25, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveRight)));
    REQUIRE(widthOverrides.size() == 1);
    CHECK(widthOverrides[0].field == rt::TrackField::Title);
    CHECK(widthOverrides[0].columns == 25);
    CHECK(controller.statusMessage() == "Column width updated");

    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 25, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", release)));
    REQUIRE(widthOverrides.size() == 1);
    CHECK(widthOverrides[0].columns == 25);

    auto moveAfterRelease = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 2};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", moveAfterRelease)));
    CHECK(widthOverrides[0].columns == 25);

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressEdge)));
    auto moveFarLeft = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = -100, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveFarLeft)));
    CHECK(widthOverrides[0].columns == kMinimumTrackColumnWidthColumns);
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", release)));

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressEdge)));
    auto moveFarRight = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 220, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveFarRight)));
    CHECK(widthOverrides[0].columns == kMaximumTrackColumnResizeColumns);
  }

  TEST_CASE("EventController - interrupted column drag does not swallow the next press", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto resizeHandles =
      std::vector{TrackColumnResizeHandle{.field = rt::TrackField::Title,
                                          .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                                          .columns = 20}};
    auto widthOverrides = std::vector<TrackColumnWidthOverride>{};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.trackColumnResizeHandles = &resizeHandles,
                                                              .trackColumnWidthOverrides = &widthOverrides}};

    auto pressEdge = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressEdge)));

    auto secondPress = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 2, .y = 8};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", secondPress)));

    auto staleMove = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 40, .y = 2};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", staleMove)));
    CHECK(widthOverrides.empty());
  }

  TEST_CASE("EventController - mouse wheel scrolls the track table", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto tableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.trackTableBox = &tableBox}};

    REQUIRE(library.selectedTrack() == 0);

    auto wheelDown = ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", wheelDown)));
    CHECK(library.selectedTrack() == 1);

    auto wheelUp = ftxui::Mouse{.button = ftxui::Mouse::WheelUp, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", wheelUp)));
    CHECK(library.selectedTrack() == 0);

    auto wheelOutside =
      ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 23};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", wheelOutside)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - mouse drag scrolls the track table scrollbar", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto tableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.trackTableBox = &tableBox}};

    REQUIRE(library.selectedTrack() == 0);

    auto pressBottom = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 22};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressBottom)));
    CHECK(library.selectedTrack() == 1);

    auto dragTop = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 79, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", dragTop)));
    CHECK(library.selectedTrack() == 0);

    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 79, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", release)));
    CHECK(library.selectedTrack() == 0);

    auto dragAfterRelease = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 79, .y = 22};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", dragAfterRelease)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - scrollbar handles one-row and interrupted drags", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto tableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 2};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.trackTableBox = &tableBox}};

    CHECK(controller.handleEvent(ftxui::Event::End));
    REQUIRE(library.selectedTrack() == 1);

    auto pressOnlyBodyRow =
      ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressOnlyBodyRow)));
    CHECK(library.selectedTrack() == 0);

    auto secondPress = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 5, .y = 2};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", secondPress)));

    auto staleDrag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 79, .y = 2};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", staleDrag)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - scrollbar ignores empty track tables", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};
    enterCommand(controller, "missing");
    REQUIRE(library.tracks().empty());

    auto tableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controllerWithTable = EventController{fixture.screen,
                                               fixture.shell,
                                               library,
                                               fixture.runtime.playback(),
                                               EventControllerBindings{.trackTableBox = &tableBox}};

    auto pressScrollbar = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 22};
    CHECK_FALSE(controllerWithTable.handleEvent(ftxui::Event::Mouse("", pressScrollbar)));
  }

  TEST_CASE("EventController - section shortcuts jump between grouped sections", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    library::test::addTrack(
      fixture.runtime.musicLibrary(),
      library::test::TrackSpec{
        .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto const expected = library.sections()[1];
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("}")));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(expected.rowBegin));
    CHECK(controller.statusMessage() == "Section: " + expected.primaryText);

    CHECK(controller.handleEvent(ftxui::Event::Character("{")));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - section shortcuts do not pass through overlays", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    library::test::addTrack(
      fixture.runtime.musicLibrary(),
      library::test::TrackSpec{
        .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    fixture.shell.openOverlay(Overlay::ListChooser);
    CHECK(controller.handleEvent(ftxui::Event::Character("}")));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - mouse clicks section headers jump to grouped sections", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    library::test::addTrack(
      fixture.runtime.musicLibrary(),
      library::test::TrackSpec{
        .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto const expected = library.sections()[1];
    auto sectionBoxes = std::vector{
      TrackSectionRowBox{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.trackSectionRowBoxes = &sectionBoxes}};

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(expected.rowBegin));
    CHECK(controller.statusMessage() == "Section: " + expected.primaryText);
  }

  TEST_CASE("EventController - stale section header clicks report unavailable sections", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    REQUIRE(library.sections().empty());
    auto sectionBoxes = std::vector{
      TrackSectionRowBox{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.trackSectionRowBoxes = &sectionBoxes}};

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == 0);
    CHECK(controller.statusMessage() == "Section is no longer available");
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
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};
    auto seekEvents = std::vector<std::chrono::milliseconds>{};
    auto sub = fixture.runtime.playback().onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                       { seekEvents.push_back(event.elapsed); });

    fixture.runtime.playback().setVolume(0.50F);

    CHECK(controller.handleEvent(ftxui::Event::Character("[")));
    CHECK(controller.handleEvent(ftxui::Event::Character("]")));
    REQUIRE(seekEvents.size() == 2);
    CHECK(seekEvents[0] == std::chrono::milliseconds{0});
    CHECK(seekEvents[1] == std::chrono::seconds{5});

    CHECK(controller.handleEvent(ftxui::Event::Character("-")));
    CHECK(fixture.runtime.playback().state().volume.level < 0.50F);

    CHECK(controller.handleEvent(ftxui::Event::Character("+")));
    CHECK(fixture.runtime.playback().state().volume.level > 0.49F);

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
