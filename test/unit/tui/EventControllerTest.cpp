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
#include "tui/TrackListEntry.h"
#include "tui/TrackPresentationNavigation.h"
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
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    std::unique_ptr<async::Executor> makeQueuedExecutor(rt::test::QueuedExecutor*& executor)
    {
      auto ownerPtr = std::make_unique<rt::test::QueuedExecutor>();
      executor = ownerPtr.get();
      return ownerPtr;
    }

    struct EventControllerFixture final
    {
      ao::test::TempDir tempDir{};
      rt::test::QueuedExecutor* executor = nullptr;
      rt::AppRuntime runtime{rt::test::makeRuntime(tempDir, makeQueuedExecutor(executor))};
      ftxui::ScreenInteractive screen{ftxui::ScreenInteractive::FixedSize(80, 24)};
      ShellInteractionModel shell{};

      EventControllerFixture()
      {
        auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
        addTrack(library::test::TrackSpec{.title = "First", .uri = fixturePath});
        addTrack(library::test::TrackSpec{.title = "Second", .uri = fixturePath});
      }

      LibraryController makeLibrary() { return LibraryController{runtime}; }

      TrackId addTrack(library::test::TrackSpec const& spec)
      {
        return rt::test::addRuntimeTrack(runtime, spec, [this] { executor->drain(); });
      }

      void addReadyAudioProvider()
      {
        rt::test::addReadyAudioProvider(runtime);
        executor->drain();
      }

      void addReadyAudioProvider(audio::BackendProvider::Status status)
      {
        rt::test::addReadyAudioProvider(runtime, std::move(status));
        executor->drain();
      }
    };

    rt::PlaybackSnapshot currentPlayback(EventControllerFixture& fixture)
    {
      return fixture.runtime.playback().snapshot();
    }

    std::int32_t presentationIndex(LibraryController const& library, std::string_view const presentationId)
    {
      auto const& entries = library.presentationEntries();
      auto const it = std::ranges::find(entries, presentationId, &TrackPresentationNavEntry::id);
      return it == entries.end() ? -1 : static_cast<std::int32_t>(it - entries.begin());
    }

    void prepareSeekablePlayback(EventControllerFixture& fixture, LibraryController const& library)
    {
      REQUIRE_FALSE(library.tracks().empty());
      fixture.addReadyAudioProvider();
      auto const startTrackId = library.tracks()[0].id;
      auto& playback = fixture.runtime.playback();
      REQUIRE(playback.commands().startFromView(library.activeViewId(), startTrackId));
      REQUIRE(playback.snapshot().transport.duration > std::chrono::milliseconds{0});
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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
      fixture.runtime,
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
            .items = {rt::CompletionItem{.displayText = "/detail",
                                         .insertText = "detail",
                                         .detail = rt::CompletionDetail::makeResolvedText("track detail")}},
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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - overlay shortcuts update visible shell state", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto outputDevices = OutputDeviceController{fixture.runtime.playback()};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime,
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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - presentation shortcut selects track views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    auto const albumsIndex = presentationIndex(library, "albums");
    REQUIRE(albumsIndex >= 0);

    for (std::int32_t index = 0; index < albumsIndex; ++index)
    {
      CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    }

    CHECK(library.selectedPresentation() == albumsIndex);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtime.views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - presentation navigation keys move within views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

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
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);

    enterCommand(controller, "toggle");
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);

    enterCommand(controller, "stop");
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("EventController - output commands and mouse clicks select devices", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
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
                      fixture.runtime,
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
    CHECK(currentPlayback(fixture).transport.output.selectedDevice.backendId == audio::BackendId{"test_backend"});

    enterCommand(controller, "output");
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(currentPlayback(fixture).transport.output.selectedDevice.backendId == audio::BackendId{"test_backend"});
  }

  TEST_CASE("EventController - stale output row clicks keep the picker open", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
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
                      fixture.runtime,
                      EventControllerBindings{.outputDevices = &outputDevices, .hitRegions = &hitRegions}};

    fixture.shell.openOverlay(Overlay::OutputDevices);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 3};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));

    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);
    CHECK(currentPlayback(fixture).transport.output.selectedDevice.backendId == audio::BackendId{"test_backend"});
  }

  TEST_CASE("EventController - output selector handles page and boundary navigation keys", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider(rt::test::makePipeWireOutputStatus());
    auto library = fixture.makeLibrary();
    auto outputDevices = OutputDeviceController{fixture.runtime.playback()};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime,
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
    auto const playbackSnapshot = currentPlayback(fixture);
    ftxui::Render(renderScreen,
                  playbackBar(PlaybackBarViewState{.playbackState = &playbackSnapshot.transport,
                                                   .soulButtonBox = &hitRegions.soulButtonBox,
                                                   .terminalColumns = 80}));
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

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
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    auto renderScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(1));
    auto const playbackSnapshot = currentPlayback(fixture);
    ftxui::Render(renderScreen,
                  playbackBar(PlaybackBarViewState{.playbackState = &playbackSnapshot.transport,
                                                   .soulButtonBox = &hitRegions.soulButtonBox,
                                                   .terminalColumns = 80}));
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    REQUIRE(hitRegions.soulButtonBox.x_min < hitRegions.soulButtonBox.x_max);

    auto clickSoulButton = ftxui::Mouse{.button = ftxui::Mouse::Left,
                                        .motion = ftxui::Mouse::Pressed,
                                        .x = hitRegions.soulButtonBox.x_min,
                                        .y = hitRegions.soulButtonBox.y_min};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Playing);
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Paused);

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Playing);
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);
  }

  TEST_CASE("EventController - unavailable transport command is gated and reported", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime,
      EventControllerBindings{.notifications = &fixture.runtime.notifications()},
    };

    CHECK(controller.handleEvent(ftxui::Event::Character(" ")));

    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Idle);
    auto const feed = fixture.runtime.notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Warning);
    CHECK(rt::resolvedNotificationText(feed.entries.back().message) == "Playback control unavailable");
  }

  TEST_CASE("EventController - idle stop is a silent no-op", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime,
      EventControllerBindings{.notifications = &fixture.runtime.notifications()},
    };

    CHECK(controller.handleEvent(ftxui::Event::Character("s")));

    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Idle);
    CHECK(fixture.runtime.notifications().feed().entries.empty());
  }

  TEST_CASE("EventController - space pauses playback while output selection is pending", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    REQUIRE_FALSE(library.tracks().empty());
    auto& playback = fixture.runtime.playback();
    auto& commands = playback.commands();
    REQUIRE(commands.startFromView(library.activeViewId(), library.tracks()[0].id));
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime,
      EventControllerBindings{.notifications = &fixture.runtime.notifications()},
    };
    auto const selected = playback.snapshot().transport.output.selectedDevice;

    commands.setOutputDevice(selected.backendId, audio::DeviceId{"pending-device"}, selected.profileId);

    auto transport = playback.snapshot().transport;
    REQUIRE_FALSE(transport.ready);
    REQUIRE(transport.transport == audio::Transport::Playing);
    auto const notificationCount = fixture.runtime.notifications().feed().entries.size();

    CHECK(controller.handleEvent(ftxui::Event::Character(" ")));

    transport = playback.snapshot().transport;
    CHECK(transport.transport == audio::Transport::Paused);
    CHECK(fixture.runtime.notifications().feed().entries.size() == notificationCount);
  }

  TEST_CASE("EventController - presentation mouse clicks open and select views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    auto const albumsIndex = presentationIndex(library, "albums");
    REQUIRE(albumsIndex >= 0);
    hitRegions.presentationButtonBox = ftxui::Box{.x_min = 20, .x_max = 29, .y_min = 23, .y_max = 23};
    hitRegions.presentationRows = {PresentationRowHitRegion{
      .rowIndex = albumsIndex, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

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
                                      fixture.runtime,
                                      EventControllerBindings{.activityStatusViewModel = &activityStatusViewModel}};

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);

    fixture.runtime.notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::sessionHistory());

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
                                      fixture.runtime,
                                      EventControllerBindings{.activityStatusViewModel = &activityStatusViewModel}};

    activityStatusViewModel.handleLibraryTaskProgress(rt::LibraryChanges::LibraryTaskProgressUpdated{
      .kind = rt::LibraryChanges::LibraryTaskProgressKind::Updating,
      .fraction = 0.625,
      .subject = "status-progress.flac",
    });
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
    auto activityStatusViewModel = uimodel::ActivityStatusViewModel{
      fixture.runtime.notifications(),
      [](uimodel::ActivityStatusViewState const&) {},
    };
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      fixture.runtime,
                                      EventControllerBindings{
                                        .activityStatusViewModel = &activityStatusViewModel,
                                        .notifications = &fixture.runtime.notifications(),
                                      }};

    CHECK(controller.handleEvent(ftxui::Event::Character("l")));

    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Info);
    CHECK(activityStatusViewModel.viewState().compact.text == "Lists");
    CHECK_FALSE(activityStatusViewModel.hasPendingAutoDismiss());
    auto const feed = fixture.runtime.notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().lifetime == rt::NotificationLifetime::transient());
  }

  TEST_CASE("EventController - notification mouse targets hide only activity presentation", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto activityStatusViewModel =
      uimodel::ActivityStatusViewModel{fixture.runtime.notifications(), [](uimodel::ActivityStatusViewState const&) {}};
    auto const notificationId =
      fixture.runtime.notifications()
        .post(rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::sessionHistory())
        .id;
    auto hitRegions = TuiHitRegions{};
    hitRegions.activityStatusBox = ftxui::Box{.x_min = 0, .x_max = 24, .y_min = 23, .y_max = 23};
    hitRegions.notificationDetailRows = {NotificationDetailRowHitRegion{
      .id = notificationId, .dismissible = true, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      fixture.runtime,
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
                      fixture.runtime,
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
                      fixture.runtime,
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
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};
    enterCommand(controller, "missing");
    REQUIRE(library.tracks().empty());

    auto hitRegions = TuiHitRegions{};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controllerWithTable = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    auto pressScrollbar = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 22};
    CHECK_FALSE(controllerWithTable.handleEvent(ftxui::Event::Mouse("", pressScrollbar)));
  }

  TEST_CASE("EventController - section shortcuts jump between grouped sections", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{
      .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto const expected = library.sections()[1];
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

    CHECK(controller.handleEvent(ftxui::Event::Character("}")));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(expected.rowBegin));

    CHECK(controller.handleEvent(ftxui::Event::Character("{")));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - section shortcuts do not pass through overlays", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{
      .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

    fixture.shell.openOverlay(Overlay::ListChooser);
    CHECK(controller.handleEvent(ftxui::Event::Character("}")));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - mouse clicks section headers jump to grouped sections", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{
      .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto const expected = library.sections()[1];
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackSectionRows = {
      TrackSectionRowHitRegion{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - list chooser return opens the selected list", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

    fixture.shell.openOverlay(Overlay::ListChooser);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(library.currentListTitle() == "All Tracks");
  }

  TEST_CASE("EventController - playback shortcuts update controls", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};
    auto& playback = fixture.runtime.playback();
    auto& commands = playback.commands();

    commands.setVolume(0.50F);
    commands.setMuted(true);

    CHECK(controller.handleEvent(ftxui::Event::Character("[")));
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{0});
    CHECK(controller.handleEvent(ftxui::Event::Character("]")));
    auto transport = playback.snapshot().transport;
    CHECK(transport.elapsed == transport.duration);

    CHECK(controller.handleEvent(ftxui::Event::Character("-")));
    transport = playback.snapshot().transport;
    CHECK(transport.volume.level < 0.50F);
    CHECK(transport.volume.muted);

    CHECK(controller.handleEvent(ftxui::Event::Character("+")));
    transport = playback.snapshot().transport;
    CHECK(transport.volume.level > 0.49F);
    CHECK_FALSE(transport.volume.muted);

    CHECK(controller.handleEvent(ftxui::Event::Character("s")));
    CHECK(playback.snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("EventController - relative seek is inert without a known duration", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};
    auto& playback = fixture.runtime.playback();
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot)
                                                    { snapshots.push_back(snapshot); });

    CHECK(controller.handleEvent(ftxui::Event::Character("[")));
    CHECK(controller.handleEvent(ftxui::Event::Character("]")));

    CHECK(snapshots.empty());
    CHECK(playback.snapshot().transport.duration == std::chrono::milliseconds{0});
  }

  TEST_CASE("EventController - mouse click on seek rail previews then commits the target position",
            "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& playback = fixture.runtime.playback();
    auto const duration = playback.snapshot().transport.duration;

    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<rt::PlaybackSeekPreview>{};
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](rt::PlaybackSeekPreview const& preview)
                                                      { seekPreviews.push_back(preview); });
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot)
                                                    { snapshots.push_back(snapshot); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 20, .y = 1};

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", release)));

    REQUIRE(seekPreviews.size() == 1);
    CHECK(seekPreviews[0].elapsed == duration / 2);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].transport.elapsed == duration / 2);
  }

  TEST_CASE("EventController - mouse drag on seek rail clamps release outside the rail", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& playback = fixture.runtime.playback();
    auto const duration = playback.snapshot().transport.duration;

    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<rt::PlaybackSeekPreview>{};
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](rt::PlaybackSeekPreview const& preview)
                                                      { seekPreviews.push_back(preview); });
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot)
                                                    { snapshots.push_back(snapshot); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 1};
    auto dragOutside = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 99, .y = 1};
    auto releaseOutside = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 99, .y = 1};

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", dragOutside)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", releaseOutside)));

    REQUIRE(seekPreviews.size() == 2);
    CHECK(seekPreviews[0].elapsed == std::chrono::milliseconds{0});
    CHECK(seekPreviews[1].elapsed == duration);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].transport.elapsed == duration);
  }

  TEST_CASE("EventController - disabled seek rail ignores mouse clicks", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<rt::PlaybackSeekPreview>{};
    auto previewSub = fixture.runtime.playback().events().onSeekPreview(
      [&seekPreviews](rt::PlaybackSeekPreview const& preview) { seekPreviews.push_back(preview); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekPreviews.empty());
  }

  TEST_CASE("EventController - modal overlays block seek rail mouse clicks", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<rt::PlaybackSeekPreview>{};
    auto previewSub = fixture.runtime.playback().events().onSeekPreview(
      [&seekPreviews](rt::PlaybackSeekPreview const& preview) { seekPreviews.push_back(preview); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    fixture.shell.openOverlay(Overlay::ListChooser);
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekPreviews.empty());
  }

  TEST_CASE("EventController - command mode blocks seek rail mouse clicks", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<rt::PlaybackSeekPreview>{};
    auto previewSub = fixture.runtime.playback().events().onSeekPreview(
      [&seekPreviews](rt::PlaybackSeekPreview const& preview) { seekPreviews.push_back(preview); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekPreviews.empty());
  }

  TEST_CASE("EventController - command mode blocks workspace mouse controls", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    fixture.addTrack(library::test::TrackSpec{
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
      fixture.runtime,
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
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Idle);
  }

  TEST_CASE("EventController - modal overlays cancel active seek rail drags", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto& playback = fixture.runtime.playback();
    auto seekPreviews = std::vector<rt::PlaybackSeekPreview>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](rt::PlaybackSeekPreview const& preview)
                                                      { seekPreviews.push_back(preview); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, fixture.runtime, EventControllerBindings{.hitRegions = &hitRegions}};

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 1};
    auto drag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 1};
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 30, .y = 1};

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(seekPreviews.size() == 1);
    CHECK(seekPreviews[0].elapsed == std::chrono::milliseconds{0});

    fixture.shell.openOverlay(Overlay::ListChooser);
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", drag)));
    CHECK(seekPreviews.size() == 1);
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{0});

    fixture.shell.closeOverlay();
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", release)));
    CHECK(seekPreviews.size() == 1);
  }

  TEST_CASE("EventController - current track shortcut reveals playback selection", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, fixture.runtime};

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    REQUIRE(library.selectedTrack() == 1);
    CHECK(controller.handleEvent(ftxui::Event::Character("p")));
    REQUIRE(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[1].id);

    CHECK(controller.handleEvent(ftxui::Event::Home));
    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::CtrlL));
    CHECK(library.selectedTrack() == 1);
  }
} // namespace ao::tui::test
