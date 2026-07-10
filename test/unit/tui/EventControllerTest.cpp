// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/EventController.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "tui/LibraryController.h"
#include "tui/NotificationCenterPanel.h"
#include "tui/OutputDeviceController.h"
#include "tui/OutputDevicePanel.h"
#include "tui/PlaybackPanel.h"
#include "tui/PresentationPanel.h"
#include "tui/ShellInteractionModel.h"
#include "tui/TrackSection.h"
#include "tui/TrackTable.h"
#include "tui/TuiHitRegions.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackSessionState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

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
      ShellInteractionModel shell{};

      EventControllerFixture()
      {
        auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
        library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "First", .uri = fixturePath});
        library::test::addTrack(
          runtime.musicLibrary(), library::test::TrackSpec{.title = "Second", .uri = fixturePath});
      }

      LibraryController makeLibrary() { return LibraryController{runtime}; }
    };

    void prepareSeekablePlayback(EventControllerFixture& fixture, LibraryController const& library)
    {
      REQUIRE_FALSE(library.tracks().empty());
      REQUIRE(fixture.runtime.playback().restoreSession(rt::PlaybackSessionState{
        .sourceListId = library.currentListId(),
        .trackId = library.tracks()[0].id,
      }));
      REQUIRE(fixture.runtime.playback().state().duration > std::chrono::milliseconds{0});
    }

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

    CHECK(fixture.shell.isCommandActive());
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
    CHECK(fixture.shell.isCommandActive());
    CHECK(fixture.shell.commandDraft() == "detail");
    CHECK_FALSE(fixture.shell.commandCompletion());

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.shell.isCommandActive());
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
  }

  TEST_CASE("EventController - command input escape cancels the draft", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character(":")));
    CHECK(controller.handleEvent(ftxui::Event::Character("h")));
    CHECK(fixture.shell.isCommandActive());
    CHECK(fixture.shell.commandDraft() == "h");

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isCommandActive());
    CHECK(fixture.shell.commandDraft().empty());
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

  TEST_CASE("EventController - overlay shortcuts update visible shell state", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("l")));
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.handleEvent(ftxui::Event::Character("?")));
    CHECK(fixture.shell.overlay() == Overlay::Help);

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.handleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::QualityPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::None);
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

    CHECK(controller.handleEvent(ftxui::Event::Character("o")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - modal overlays swallow workspace shortcuts", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.handleEvent(ftxui::Event::Character("a")));
    REQUIRE(fixture.shell.overlay() == Overlay::QualityPanel);

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
      CHECK(fixture.shell.overlay() == Overlay::QualityPanel);
    }

    CHECK(controller.handleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::None);
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
  }

  TEST_CASE("EventController - presentation shortcut selects track views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    CHECK(library.selectedPresentation() == 2);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtime.views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - presentation navigation keys move within views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(controller.handleEvent(ftxui::Event::End));
    CHECK(library.selectedPresentation() == static_cast<std::int32_t>(library.presentationEntries().size()) - 1);

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
    CHECK_FALSE(fixture.shell.isCommandActive());
    CHECK(library.filterDraft() == "First");
    CHECK(library.tracks().size() == 1);
    REQUIRE(library.selectedTrackView().track != nullptr);
    CHECK(library.selectedTrackView().track->row.title == "First");

    enterCommand(controller, "clear");
    CHECK(library.filterDraft().empty());
    CHECK(library.tracks().size() == 2);
  }

  TEST_CASE("EventController - named commands route to shell playback and library actions", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime.playback()};

    enterCommand(controller, "lists");
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "detail");
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "quality");
    CHECK(fixture.shell.overlay() == Overlay::QualityPanel);
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "views");
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "close");
    CHECK(fixture.shell.overlay() == Overlay::None);

    enterCommand(controller, "help");
    CHECK(fixture.shell.overlay() == Overlay::Help);
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    enterCommand(controller, "current");
    CHECK(library.selectedTrack() == 0);

    enterCommand(controller, "view albums");
    CHECK(fixture.runtime.views().trackListState(library.activeViewId()).presentation.id == "albums");

    enterCommand(controller, "reload");
    CHECK(library.tracks().size() == 2);

    enterCommand(controller, "play");
    CHECK(fixture.runtime.playback().state().nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);

    enterCommand(controller, "toggle");
    CHECK(fixture.runtime.playback().state().nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);

    enterCommand(controller, "stop");
    CHECK(fixture.runtime.playback().state().nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("EventController - output commands and mouse clicks select devices", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto outputDevices = OutputDeviceController{fixture.runtime.playback()};
    outputDevices.refresh();
    REQUIRE(outputDevices.viewState().rows.size() > 1);
    auto const outputRow = outputDevices.viewState().rows[1];
    auto hitRegions = TuiHitRegions{};
    hitRegions.outputDeviceButtonBox = ftxui::Box{.x_min = 4, .x_max = 9, .y_min = 0, .y_max = 0};
    hitRegions.outputDeviceRows = {
      OutputDeviceRowHitRegion{.rowIndex = 1,
                               .backendId = outputRow.backendId,
                               .deviceId = outputRow.deviceId,
                               .profileId = outputRow.profileId,
                               .box = ftxui::Box{.x_min = 2, .x_max = 30, .y_min = 3, .y_max = 3}}};
    auto controller =
      EventController{fixture.screen,
                      fixture.shell,
                      library,
                      fixture.runtime.playback(),
                      EventControllerBindings{.outputDevices = &outputDevices, .hitRegions = &hitRegions}};

    auto clickBadge = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 6, .y = 0};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickBadge)));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    auto clickOrigin = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 0, .y = 0};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", clickOrigin)));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 3};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtime.playback().state().output.selectedDevice.backendId == audio::BackendId{"test_backend"});

    enterCommand(controller, "output");
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtime.playback().state().output.selectedDevice.backendId == audio::BackendId{"test_backend"});
  }

  TEST_CASE("EventController - stale output row clicks keep the picker open", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto outputDevices = OutputDeviceController{fixture.runtime.playback()};
    outputDevices.refresh();
    auto hitRegions = TuiHitRegions{};
    hitRegions.outputDeviceRows = {
      OutputDeviceRowHitRegion{.rowIndex = 1,
                               .backendId = audio::BackendId{"stale_backend"},
                               .deviceId = audio::DeviceId{"stale_device"},
                               .profileId = audio::kProfileShared,
                               .box = ftxui::Box{.x_min = 2, .x_max = 30, .y_min = 3, .y_max = 3}}};
    auto controller =
      EventController{fixture.screen,
                      fixture.shell,
                      library,
                      fixture.runtime.playback(),
                      EventControllerBindings{.outputDevices = &outputDevices, .hitRegions = &hitRegions}};

    fixture.shell.openOverlay(Overlay::OutputDevices);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 3};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));

    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);
    CHECK(fixture.runtime.playback().state().output.selectedDevice.backendId == audio::BackendId{"test_backend"});
  }

  TEST_CASE("EventController - output selector handles page and boundary navigation keys", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback(), rt::test::makePipeWireOutputStatus());
    auto library = fixture.makeLibrary();
    auto outputDevices = OutputDeviceController{fixture.runtime.playback()};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.outputDevices = &outputDevices}};

    REQUIRE(outputDevices.selectedRow() == 1);
    CHECK(controller.handleEvent(ftxui::Event::Character("o")));
    REQUIRE(fixture.shell.overlay() == Overlay::OutputDevices);

    CHECK(controller.handleEvent(ftxui::Event::PageDown));
    CHECK(outputDevices.selectedRow() == 2);
    CHECK(controller.handleEvent(ftxui::Event::PageUp));
    CHECK(outputDevices.selectedRow() == 1);
    CHECK(controller.handleEvent(ftxui::Event::End));
    CHECK(outputDevices.selectedRow() == 2);
    CHECK(controller.handleEvent(ftxui::Event::Home));
    CHECK(outputDevices.selectedRow() == 1);
  }

  TEST_CASE("EventController - hovering the soul button shows transient quality details", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    auto renderScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(1));
    ftxui::Render(renderScreen,
                  playbackBar(PlaybackBarViewState{.playbackState = &fixture.runtime.playback().state(),
                                                   .soulButtonBox = &hitRegions.soulButtonBox,
                                                   .terminalColumns = 80}));
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    REQUIRE(hitRegions.soulButtonBox.x_min == 0);
    REQUIRE(hitRegions.soulButtonBox.x_max > hitRegions.soulButtonBox.x_min);

    auto moveOverButton = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 0, .y = 0};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveOverButton)));
    CHECK(controller.isQualityHoverVisible());
    CHECK(fixture.shell.overlay() == Overlay::None);

    auto moveAway = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 20, .y = 0};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveAway)));
    CHECK_FALSE(controller.isQualityHoverVisible());

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", moveOverButton)));
    CHECK_FALSE(controller.isQualityHoverVisible());
  }

  TEST_CASE("EventController - hovering clickable buttons updates hover target", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    hitRegions.outputDeviceButtonBox = ftxui::Box{.x_min = 4, .x_max = 9, .y_min = 0, .y_max = 0};
    hitRegions.libraryButtonBox = ftxui::Box{.x_min = 2, .x_max = 12, .y_min = 23, .y_max = 23};
    hitRegions.presentationButtonBox = ftxui::Box{.x_min = 15, .x_max = 24, .y_min = 23, .y_max = 23};
    hitRegions.activityStatusBox = ftxui::Box{.x_min = 28, .x_max = 48, .y_min = 23, .y_max = 23};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    auto moveOutput = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 5, .y = 0};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveOutput)));
    CHECK(controller.hoveredButton() == HoveredButton::OutputDevice);

    auto moveLibrary = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 3, .y = 23};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveLibrary)));
    CHECK(controller.hoveredButton() == HoveredButton::Library);

    auto movePresentation = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 16, .y = 23};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", movePresentation)));
    CHECK(controller.hoveredButton() == HoveredButton::Presentation);

    auto moveActivity = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 23};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveActivity)));
    CHECK(controller.hoveredButton() == HoveredButton::ActivityStatus);

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveActivity)));
    CHECK(controller.hoveredButton() == HoveredButton::None);
  }

  TEST_CASE("EventController - clicking the soul button toggles playback", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    auto renderScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(1));
    ftxui::Render(renderScreen,
                  playbackBar(PlaybackBarViewState{.playbackState = &fixture.runtime.playback().state(),
                                                   .soulButtonBox = &hitRegions.soulButtonBox,
                                                   .terminalColumns = 80}));
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    REQUIRE(hitRegions.soulButtonBox.x_min < hitRegions.soulButtonBox.x_max);

    auto clickSoulButton = ftxui::Mouse{.button = ftxui::Mouse::Left,
                                        .motion = ftxui::Mouse::Pressed,
                                        .x = hitRegions.soulButtonBox.x_min,
                                        .y = hitRegions.soulButtonBox.y_min};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    CHECK(fixture.runtime.playback().state().transport == audio::Transport::Playing);
    CHECK(fixture.runtime.playback().state().nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    CHECK(fixture.runtime.playback().state().transport == audio::Transport::Paused);
  }

  TEST_CASE("EventController - presentation mouse clicks open and select views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    hitRegions.presentationButtonBox = ftxui::Box{.x_min = 20, .x_max = 29, .y_min = 23, .y_max = 23};
    hitRegions.presentationRows = {
      PresentationRowHitRegion{.rowIndex = 2, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    auto clickView = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 24, .y = 23};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickView)));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 12};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtime.views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - notification shortcut opens available activity details", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto activityStatusViewModel =
      uimodel::ActivityStatusViewModel{fixture.runtime.notifications(), [](uimodel::ActivityStatusViewState const&) {}};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.activityStatusViewModel = &activityStatusViewModel}};

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);

    fixture.runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import");

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - notification hide shortcut respects dismissible state", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto activityStatusViewModel =
      uimodel::ActivityStatusViewModel{fixture.runtime.notifications(), [](uimodel::ActivityStatusViewState const&) {}};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.activityStatusViewModel = &activityStatusViewModel}};

    activityStatusViewModel.handleLibraryTaskProgress("Updating: status-progress.flac", 0.625);
    REQUIRE(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Processing);
    REQUIRE_FALSE(activityStatusViewModel.viewState().compact.dismissible);

    fixture.shell.openOverlay(Overlay::Notifications);

    CHECK(controller.handleEvent(ftxui::Event::Character("x")));
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Processing);
    CHECK(activityStatusViewModel.viewState().compact.text == "Updating library");
  }

  TEST_CASE("EventController - panel actions use transient activity notifications when available", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto now = std::chrono::steady_clock::time_point{};
    auto activityStatusViewModel = uimodel::ActivityStatusViewModel{
      fixture.runtime.notifications(),
      [](uimodel::ActivityStatusViewState const&) {},
      uimodel::ActivityStatusViewModelOptions{.clock = [&] { return now; }},
    };
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{
                                        .activityStatusViewModel = &activityStatusViewModel,
                                        .notifications = &fixture.runtime.notifications(),
                                      }};

    CHECK(controller.handleEvent(ftxui::Event::Character("l")));

    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Info);
    CHECK(activityStatusViewModel.viewState().compact.text == "Lists");
    CHECK(activityStatusViewModel.hasPendingAutoDismiss());

    now += uimodel::kActivityStatusDefaultAutoDismissTimeout;

    CHECK(activityStatusViewModel.expireTransientIfDue());
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Idle);
  }

  TEST_CASE("EventController - notification mouse targets hide only activity presentation", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto activityStatusViewModel =
      uimodel::ActivityStatusViewModel{fixture.runtime.notifications(), [](uimodel::ActivityStatusViewState const&) {}};
    auto const notificationId =
      fixture.runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import");
    auto hitRegions = TuiHitRegions{};
    hitRegions.activityStatusBox = ftxui::Box{.x_min = 0, .x_max = 24, .y_min = 23, .y_max = 23};
    hitRegions.notificationDetailRows = {NotificationDetailRowHitRegion{
      .id = notificationId, .dismissible = true, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime.playback(),
      EventControllerBindings{.hitRegions = &hitRegions, .activityStatusViewModel = &activityStatusViewModel}};

    auto clickActivity = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 8, .y = 23};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickActivity)));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 12};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(activityStatusViewModel.viewState().detail.items.empty());
    CHECK(fixture.runtime.notifications().feed().entries.size() == 1);
  }

  TEST_CASE("EventController - mouse drag resizes track columns in session state", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20}};
    auto widthOverrides = std::vector<TrackColumnWidthOverride>{};
    auto controller =
      EventController{fixture.screen,
                      fixture.shell,
                      library,
                      fixture.runtime.playback(),
                      EventControllerBindings{.hitRegions = &hitRegions, .trackColumnWidthOverrides = &widthOverrides}};

    auto pressEdge = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressEdge)));

    auto moveRight = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 25, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveRight)));
    REQUIRE(widthOverrides.size() == 1);
    CHECK(widthOverrides[0].field == rt::TrackField::Title);
    CHECK(widthOverrides[0].columns == 25);

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20}};
    auto widthOverrides = std::vector<TrackColumnWidthOverride>{};
    auto controller =
      EventController{fixture.screen,
                      fixture.shell,
                      library,
                      fixture.runtime.playback(),
                      EventControllerBindings{.hitRegions = &hitRegions, .trackColumnWidthOverrides = &widthOverrides}};

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 2};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

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

    auto hitRegions = TuiHitRegions{};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controllerWithTable = EventController{fixture.screen,
                                               fixture.shell,
                                               library,
                                               fixture.runtime.playback(),
                                               EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackSectionRows = {
      TrackSectionRowHitRegion{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(expected.rowBegin));
  }

  TEST_CASE("EventController - stale section header clicks report unavailable sections", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    REQUIRE(library.sections().empty());
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackSectionRows = {
      TrackSectionRowHitRegion{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == 0);
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
  }

  TEST_CASE("EventController - playback shortcuts update controls", "[tui][unit][event]")
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
    CHECK(fixture.runtime.playback().state().nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("EventController - mouse click on seek rail previews then commits the target position",
            "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto const duration = fixture.runtime.playback().state().duration;

    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
    auto sub = fixture.runtime.playback().onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                       { seekEvents.push_back(event); });
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 20, .y = 1};

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", release)));

    REQUIRE(seekEvents.size() == 2);
    CHECK(seekEvents[0].mode == rt::PlaybackService::SeekMode::Preview);
    CHECK(seekEvents[0].elapsed == duration / 2);
    CHECK(seekEvents[1].mode == rt::PlaybackService::SeekMode::Final);
    CHECK(seekEvents[1].elapsed == duration / 2);
  }

  TEST_CASE("EventController - mouse drag on seek rail clamps release outside the rail", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto const duration = fixture.runtime.playback().state().duration;

    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
    auto sub = fixture.runtime.playback().onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                       { seekEvents.push_back(event); });
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 1};
    auto dragOutside = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 99, .y = 1};
    auto releaseOutside = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 99, .y = 1};

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", dragOutside)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", releaseOutside)));

    REQUIRE(seekEvents.size() == 3);
    CHECK(seekEvents[0].mode == rt::PlaybackService::SeekMode::Preview);
    CHECK(seekEvents[0].elapsed == std::chrono::milliseconds{0});
    CHECK(seekEvents[1].mode == rt::PlaybackService::SeekMode::Preview);
    CHECK(seekEvents[1].elapsed == duration);
    CHECK(seekEvents[2].mode == rt::PlaybackService::SeekMode::Final);
    CHECK(seekEvents[2].elapsed == duration);
  }

  TEST_CASE("EventController - disabled seek rail ignores mouse clicks", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
    auto sub = fixture.runtime.playback().onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                       { seekEvents.push_back(event); });
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekEvents.empty());
  }

  TEST_CASE("EventController - modal overlays block seek rail mouse clicks", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
    auto sub = fixture.runtime.playback().onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                       { seekEvents.push_back(event); });
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    fixture.shell.openOverlay(Overlay::ListChooser);
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekEvents.empty());
  }

  TEST_CASE("EventController - command mode blocks seek rail mouse clicks", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
    auto sub = fixture.runtime.playback().onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                       { seekEvents.push_back(event); });
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekEvents.empty());
  }

  TEST_CASE("EventController - command mode blocks workspace mouse controls", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime.playback());
    library::test::addTrack(
      fixture.runtime.musicLibrary(),
      library::test::TrackSpec{
        .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto outputDevices = OutputDeviceController{fixture.runtime.playback()};
    auto hitRegions = TuiHitRegions{};
    hitRegions.outputDeviceButtonBox = ftxui::Box{.x_min = 4, .x_max = 9, .y_min = 0, .y_max = 0};
    hitRegions.soulButtonBox = ftxui::Box{.x_min = 0, .x_max = 2, .y_min = 0, .y_max = 0};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20}};
    hitRegions.trackSectionRows = {
      TrackSectionRowHitRegion{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto widthOverrides = std::vector<TrackColumnWidthOverride>{};
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime.playback(),
      EventControllerBindings{
        .outputDevices = &outputDevices, .hitRegions = &hitRegions, .trackColumnWidthOverrides = &widthOverrides}};

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    REQUIRE(fixture.shell.isCommandActive());

    auto clickSoul = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 1, .y = 0};
    auto clickOutput = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 6, .y = 0};
    auto wheelDown = ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    auto pressResize = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    auto dragResize = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 2};
    auto pressScrollbar = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 22};
    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", clickSoul)));
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", clickOutput)));
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", wheelDown)));
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", pressResize)));
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", dragResize)));
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", pressScrollbar)));
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));

    CHECK(fixture.shell.isCommandActive());
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(library.selectedTrack() == 0);
    CHECK(widthOverrides.empty());
    CHECK(fixture.runtime.playback().state().transport == audio::Transport::Idle);
  }

  TEST_CASE("EventController - modal overlays cancel active seek rail drags", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
    auto sub = fixture.runtime.playback().onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                       { seekEvents.push_back(event); });
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime.playback(),
                                      EventControllerBindings{.hitRegions = &hitRegions}};

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 1};
    auto drag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 1};
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 30, .y = 1};

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(seekEvents.size() == 1);

    fixture.shell.openOverlay(Overlay::ListChooser);
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", drag)));
    REQUIRE(seekEvents.size() == 2);
    CHECK(seekEvents[1].mode == rt::PlaybackService::SeekMode::Final);
    CHECK(seekEvents[1].elapsed == std::chrono::milliseconds{0});

    fixture.shell.closeOverlay();
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", release)));
    CHECK(seekEvents.size() == 2);
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
    REQUIRE(fixture.runtime.playback().state().nowPlaying.trackId == library.tracks()[1].id);

    CHECK(controller.handleEvent(ftxui::Event::Home));
    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::CtrlL));
    CHECK(library.selectedTrack() == 1);
  }
} // namespace ao::tui::test
