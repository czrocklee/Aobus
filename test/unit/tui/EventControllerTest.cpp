// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/EventController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/CommandPalettePanel.h"
#include "tui/HitRegions.h"
#include "tui/Keymap.h"
#include "tui/LibraryController.h"
#include "tui/LibraryScanController.h"
#include "tui/MouseBindings.h"
#include "tui/NavigationPanel.h"
#include "tui/NotificationCenterPanel.h"
#include "tui/OutputDeviceController.h"
#include "tui/OutputDevicePanel.h"
#include "tui/PlaybackPanel.h"
#include "tui/Preferences.h"
#include "tui/PresentationPanel.h"
#include "tui/SettingsEditor.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include "tui/TerminalTrackColumnLayout.h"
#include "tui/TrackEditController.h"
#include "tui/TrackListEntry.h"
#include "tui/TrackPresentationNavigation.h"
#include "tui/TrackPropertiesEditor.h"
#include "tui/TrackSection.h"
#include "tui/TrackTable.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
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
      std::unique_ptr<rt::test::ControlledSleeper> sleeperPtr{};
      rt::test::QueuedExecutor* executor = nullptr;
      std::unique_ptr<rt::AppRuntime> runtimePtr;
      uimodel::TrackPresentationCatalog presentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
      uimodel::ListPresentations listPresentations{presentationCatalog, runtimePtr->library().changes()};
      ShellInteractionModel shell{};
      HitRegions hitRegions{};
      std::int32_t layoutCheckpointCount = 0;
      uimodel::TrackColumnLayouts trackColumnLayouts{runtimePtr->library().changes()};
      TrackColumnResizePreview trackColumnResizePreview{};
      OutputDeviceController outputDevices{runtimePtr->playback(),
                                           ao::test::englishMessageCatalog(),
                                           uimodel::OutputDeviceIntent::discarded()};
      uimodel::ActivityStatusViewModel activityStatusViewModel{runtimePtr->notifications(),
                                                               ao::test::englishMessageCatalog(),
                                                               [](uimodel::ActivityStatusViewState const&) {}};
      std::unique_ptr<LibraryScanController> libraryScanPtr{};
      std::unique_ptr<TrackEditController> trackEditPtr{};
      Preferences preferences{};
      uimodel::KeymapModel settingsKeymap{defaultKeymap()};
      std::unique_ptr<SettingsEditor> settingsPtr;
      std::size_t exitRequestCount = 0;
      bool exitWaiting = false;

      explicit EventControllerFixture(bool const useControlledSleeper = false)
        : sleeperPtr{useControlledSleeper ? std::make_unique<rt::test::ControlledSleeper>() : nullptr}
        , runtimePtr{rt::test::makeRuntime(tempDir,
                                           makeQueuedExecutor(executor),
                                           nullptr,
                                           sleeperPtr == nullptr ? nullptr : sleeperPtr.get())}
      {
        auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
        addTrack(library::test::TrackSpec{.title = "First", .uri = fixturePath});
        addTrack(library::test::TrackSpec{.title = "Second", .uri = fixturePath});
      }

      LibraryController makeLibrary()
      {
        return LibraryController{runtimePtr->library(),
                                 runtimePtr->views(),
                                 runtimePtr->workspace(),
                                 ao::test::englishMessageCatalog(),
                                 listPresentations};
      }

      /// Every collaborator an EventController requires, all owned by this fixture.
      EventController makeEvents(LibraryController& library,
                                 KeymapPlan const& keymapPlan = defaultKeymapPlan(),
                                 InputCompletionCallback commandCompletion = {},
                                 InputCompletionCallback filterCompletion = {})
      {
        if (libraryScanPtr == nullptr)
        {
          libraryScanPtr = std::make_unique<LibraryScanController>(runtimePtr->async(),
                                                                   runtimePtr->library().jobs(),
                                                                   runtimePtr->notifications(),
                                                                   ao::test::englishMessageCatalog());
        }

        if (trackEditPtr == nullptr)
        {
          trackEditPtr = std::make_unique<TrackEditController>(runtimePtr->async(),
                                                               runtimePtr->library(),
                                                               runtimePtr->notifications(),
                                                               ao::test::englishMessageCatalog(),
                                                               TrackEditController::Outputs{},
                                                               runtimePtr->completion(),
                                                               runtimePtr->textOrderingPolicy());
        }

        settingsPtr = std::make_unique<SettingsEditor>(
          ao::test::englishMessageCatalog(),
          preferences,
          settingsKeymap,
          SettingsEditor::Outputs{.applyPreferences = [&](Preferences const& candidate) -> Result<>
                                  {
                                    preferences = candidate;
                                    return {};
                                  },
                                  .applyKeymap = [&](uimodel::KeymapModel const& candidate) -> Result<>
                                  {
                                    settingsKeymap = candidate;
                                    return {};
                                  },
                                  .coverMode = [] { return std::string{"off"}; }});
        hitRegions.trackTableRevision = library.trackRowsRevision();
        return EventController{shell,
                               library,
                               runtimePtr->async(),
                               runtimePtr->playback(),
                               keymapPlan,
                               EventControllerBindings{
                                 .outputDevices = outputDevices,
                                 .hitRegions = hitRegions,
                                 .trackColumnLayouts = trackColumnLayouts,
                                 .trackColumnResizePreview = trackColumnResizePreview,
                                 .activityStatusViewModel = activityStatusViewModel,
                                 .notifications = runtimePtr->notifications(),
                                 .libraryScan = *libraryScanPtr,
                                 .trackEdit = *trackEditPtr,
                                 .settings = *settingsPtr,
                                 .preferences = preferences,
                                 .requestExit = [this] { ++exitRequestCount; },
                                 .isExitWaiting = [this] { return exitWaiting; },
                                 .commandCompletionCallback = std::move(commandCompletion),
                                 .filterCompletionCallback = std::move(filterCompletion),
                                 .requestLayoutCheckpoint = [this] { ++layoutCheckpointCount; },
                               }};
      }

      TrackId addTrack(library::test::TrackSpec const& spec) const
      {
        return rt::test::addRuntimeTrack(*runtimePtr, spec, [this] { executor->drain(); });
      }

      ListId addList(std::string name) const
      {
        return ao::test::requireValue(rt::test::runRuntimeTask(
          *runtimePtr, runtimePtr->library().commands().createListAsync(rt::ListDraft{.name = std::move(name)})));
      }

      void addReadyAudioProvider() const
      {
        rt::test::addReadyAudioProvider(*runtimePtr);
        executor->drain();
      }

      void addReadyAudioProvider(audio::BackendProvider::Status status) const
      {
        rt::test::addReadyAudioProvider(*runtimePtr, std::move(status));
        executor->drain();
      }

      bool tryWaitForPlayback(TrackId const trackId)
      {
        auto const settled = rt::test::tryWaitForPlaybackSettlement(
          *executor,
          observedPositionRevision,
          [this] { return runtimePtr->playback().snapshot().transport.positionRevision; });
        observedPositionRevision = runtimePtr->playback().snapshot().transport.positionRevision;
        return settled && runtimePtr->playback().snapshot().transport.nowPlaying.trackId == trackId;
      }

      rt::PlaybackPositionRevision observedPositionRevision{};
    };

    rt::PlaybackSnapshot currentPlayback(EventControllerFixture& fixture)
    {
      return fixture.runtimePtr->playback().snapshot();
    }

    std::int32_t presentationIndex(LibraryController const& library, std::string_view const presentationId)
    {
      auto const& entries = library.presentationEntries();
      auto const it = std::ranges::find(entries, presentationId, &TrackPresentationNavEntry::id);
      return it == entries.end() ? -1 : static_cast<std::int32_t>(it - entries.begin());
    }

    void openList(LibraryController& library, ListId const listId)
    {
      REQUIRE(library.openList(listId));
      REQUIRE(library.currentListId() == listId);
    }

    void prepareSeekablePlayback(EventControllerFixture& fixture, LibraryController const& library)
    {
      REQUIRE_FALSE(library.tracks().empty());
      fixture.addReadyAudioProvider();
      auto const startTrackId = library.tracks()[0].id;
      auto& playback = fixture.runtimePtr->playback();
      REQUIRE(playback.commands().startFromView(library.activeViewId(), startTrackId));
      REQUIRE(fixture.tryWaitForPlayback(startTrackId));
      REQUIRE(playback.snapshot().transport.duration > std::chrono::milliseconds{0});
    }

    void enterCommand(EventController& controller, std::string_view text)
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::Character(":")));

      for (char const ch : text)
      {
        CHECK(controller.tryHandleEvent(ftxui::Event::Character(std::string{ch})));
      }

      CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    }

    void enterQuickFilter(EventController& controller, std::string_view text)
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));

      for (char const ch : text)
      {
        CHECK(controller.tryHandleEvent(ftxui::Event::Character(std::string{ch})));
      }

      CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    }

    std::optional<rt::CompletionResult> completeYuduo(std::string_view const draft, std::size_t /*unused*/)
    {
      if (draft != "yuduo")
      {
        return std::nullopt;
      }

      return rt::CompletionResult{
        .replaceBegin = 0,
        .replaceEnd = draft.size(),
        .items = {rt::CompletionItem{.displayText = "宇多田光", .insertText = "\"宇多田光\""}},
      };
    }
  } // namespace

  TEST_CASE("EventController - text input is modal for navigation keys", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::ArrowDown));

    CHECK(fixture.shell.isInputActive());
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - Ctrl-C requests exit without stopping playback from text input",
            "[tui][regression][event]")
  {
    auto requireExitHandlingFromInput = [](std::string const& opener)
    {
      auto fixture = EventControllerFixture{};
      auto library = fixture.makeLibrary();
      prepareSeekablePlayback(fixture, library);
      auto controller = fixture.makeEvents(library);
      auto const trackId = currentPlayback(fixture).transport.nowPlaying.trackId;

      REQUIRE(trackId != kInvalidTrackId);
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(opener)));
      REQUIRE(fixture.shell.isInputActive());

      CHECK(controller.tryHandleEvent(ftxui::Event::CtrlC));
      CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == trackId);
    };

    SECTION("Quick Filter")
    {
      requireExitHandlingFromInput("/");
    }

    SECTION("Command Palette")
    {
      requireExitHandlingFromInput(":");
    }
  }

  TEST_CASE("EventController - quit defers playback stop to the composition root", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto controller = fixture.makeEvents(library);
    auto const trackId = currentPlayback(fixture).transport.nowPlaying.trackId;
    REQUIRE(trackId != kInvalidTrackId);

    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Character("q")));
    CHECK(fixture.exitRequestCount == 0);
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("Q")));

    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == trackId);
    CHECK(fixture.exitRequestCount == 1);
  }

  TEST_CASE("EventController - Ctrl-C and quit request the shared exit callback", "[tui][unit][event][exit]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Character("q")));
    CHECK(fixture.exitRequestCount == 0);
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("Q")));
    CHECK(fixture.exitRequestCount == 1);

    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlC));
    CHECK(fixture.exitRequestCount == 2);
    enterCommand(controller, "quit");
    CHECK(fixture.exitRequestCount == 3);
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Custom));
  }

  TEST_CASE("EventController - scan commands start and cancel the library scan", "[tui][unit][event][scan]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    enterCommand(controller, "scan");
    CHECK(fixture.libraryScanPtr->phase() == LibraryScanController::Phase::Running);

    enterCommand(controller, "scan cancel");
    CHECK(fixture.libraryScanPtr->phase() == LibraryScanController::Phase::Cancelling);
    fixture.executor->drain();
  }

  TEST_CASE("EventController - select commands mark and clear tracks", "[tui][unit][event][selection]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    REQUIRE(library.tracks().size() >= 2);

    enterCommand(controller, "select toggle");
    CHECK(library.markedIds().size() == 1);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("m")));
    CHECK(library.markedIds().empty());

    enterCommand(controller, "select all");
    CHECK(library.markedIds().size() == library.tracks().size());

    enterCommand(controller, "select clear");
    CHECK(library.markedIds().empty());
  }

  TEST_CASE("EventController - Escape cancels a visual selection unless a modal overlay holds the keys",
            "[tui][unit][event][selection]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    REQUIRE(library.tracks().size() >= 2);

    SECTION("with no overlay open the selection is cancelled and the marks it started from return")
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("v")));
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("j")));
      REQUIRE(library.isVisualSelectionActive());
      REQUIRE(library.selectedTrack() == 1);
      REQUIRE(library.markedIds().size() == 2);

      CHECK(controller.tryHandleEvent(ftxui::Event::Character("k")));
      CHECK(library.selectedTrack() == 0);
      CHECK(library.markedIds().size() == 1);

      CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
      CHECK_FALSE(library.isVisualSelectionActive());
      CHECK(library.markedIds().empty());
      CHECK(fixture.shell.overlay() == Overlay::None);
    }

    SECTION("the detail panel leaves the keys to the workspace, so the selection is cancelled first")
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("d")));
      REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("v")));
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("j")));
      REQUIRE(library.isVisualSelectionActive());

      CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
      CHECK_FALSE(library.isVisualSelectionActive());
      CHECK(library.markedIds().empty());
      CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

      CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
      CHECK(fixture.shell.overlay() == Overlay::None);
    }

    SECTION("a modal overlay took those keys, so it closes first and the selection survives")
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("v")));
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("j")));
      REQUIRE(library.isVisualSelectionActive());
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("l")));
      REQUIRE(fixture.shell.isNavigationFocused());

      CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
      CHECK(fixture.shell.overlay() == Overlay::None);
      CHECK(library.isVisualSelectionActive());
      CHECK(library.markedIds().size() == 2);

      CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
      CHECK_FALSE(library.isVisualSelectionActive());
      CHECK(library.markedIds().empty());
    }
  }

  TEST_CASE("EventController - the edit command opens one editor over the whole selection",
            "[tui][unit][event][editor]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    REQUIRE(library.tracks().size() >= 2);

    enterCommand(controller, "select all");
    enterCommand(controller, "edit");

    REQUIRE(fixture.trackEditPtr->isActive());
    CHECK(fixture.trackEditPtr->activeEditor()->targetCount() == library.tracks().size());
    // Opening retires the command line it was launched from.
    CHECK_FALSE(fixture.shell.isInputActive());
    // The marks the editor captured are the workspace's own, and it keeps them.
    CHECK(library.markedIds().size() == library.tracks().size());
  }

  TEST_CASE("EventController - opening the editor ends the visual range it captured", "[tui][unit][event][editor]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    REQUIRE(library.tracks().size() >= 2);

    controller.tryHandleEvent(ftxui::Event::Character("v"));
    controller.tryHandleEvent(ftxui::Event::Character("j"));
    REQUIRE(library.isVisualSelectionActive());
    auto const captured = library.selectedTrackIds();
    REQUIRE(captured.size() == 2);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("e")));
    REQUIRE(fixture.trackEditPtr->isActive());

    // The rows the range reached are what the editor is writing to, so they
    // stay marked; only the anchor that would keep reshaping them goes.
    CHECK(library.selectedTrackIds() == captured);
    CHECK_FALSE(library.isVisualSelectionActive());

    // A motion key after the modal closes moves the focus, and no longer
    // rewrites the mark set the user left behind.
    fixture.trackEditPtr->tryHandleEvent(ftxui::Event::Escape);
    REQUIRE_FALSE(fixture.trackEditPtr->isActive());
    controller.tryHandleEvent(ftxui::Event::Character("j"));
    CHECK(library.selectedTrackIds() == captured);
  }

  TEST_CASE("EventController - the edit shortcut opens the focused track alone", "[tui][unit][event][editor]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("e")));

    REQUIRE(fixture.trackEditPtr->isActive());
    CHECK(fixture.trackEditPtr->activeEditor()->targetCount() == 1);
  }

  TEST_CASE("EventController - disabled mouse input protects an open track editor", "[tui][regression][mouse][editor]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("e")));
    REQUIRE(fixture.trackEditPtr->isActive());
    auto const* editor = fixture.trackEditPtr->activeEditor();
    auto const rendered = renderElement(editor->renderModal(80, 24), 80, 24);
    auto const optBox = findTextCells(rendered.screen, "Tags");
    REQUIRE(optBox);
    auto const click = ftxui::Event::Mouse(
      "",
      ftxui::Mouse{
        .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optBox->x_min, .y = optBox->y_min});
    fixture.preferences.mouseEnabled = false;
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(editor->tab() == TrackEditorTab::Metadata);
    CHECK_FALSE(editor->isDirty());
    fixture.preferences.mouseEnabled = true;
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(editor->tab() == TrackEditorTab::Tags);
    CHECK_FALSE(editor->isDirty());
  }

  TEST_CASE("EventController - an open editor answers for every remaining key", "[tui][unit][event][editor]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    auto const overlayBefore = fixture.shell.overlay();

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("e")));
    REQUIRE(fixture.trackEditPtr->isActive());

    // A workspace shortcut behind the editor never reaches the workspace.
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("m")));
    CHECK(library.markedIds().empty());
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == overlayBefore);

    // Escape asks to discard because typed characters edited the focused field;
    // Return confirms the discard and closes the editor.
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.trackEditPtr->isActive());
    CHECK(fixture.shell.overlay() == overlayBefore);
  }

  TEST_CASE("EventController - waiting for a submitted write consumes ordinary input", "[tui][unit][event][exit]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.exitWaiting = true;

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("m")));
    CHECK(library.markedIds().empty());
    CHECK(controller.tryHandleEvent(ftxui::Event::Character(":")));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(fixture.exitRequestCount == 0);

    // Ctrl-C is still the way out, which is what the footer advertises.
    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlC));
    CHECK(fixture.exitRequestCount == 1);
  }

  TEST_CASE("EventController - Enter on bare select remains an unknown command", "[tui][unit][event][shell]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    enterCommand(controller, "select");
    CHECK(fixture.shell.isInputActive());
    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    auto const& message = std::get<std::string>(feed.entries.back().message);
    CHECK(message.contains("select"));
  }

  TEST_CASE("EventController - cancelling untouched Quick Filter preserves the active filter",
            "[tui][regression][event][filter]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    library.setFilterDraft("First");
    REQUIRE(library.applyFilter());
    REQUIRE(library.tracks().size() == 1);
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(fixture.shell.inputMode() == ShellInputMode::QuickFilter);
    CHECK(fixture.shell.inputDraft().empty());
    CHECK_FALSE(fixture.shell.isInputTouched());
    CHECK(library.filterDraft() == "First");

    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft() == "First");
    CHECK(library.tracks().size() == 1);
  }

  TEST_CASE("EventController - confirming untouched Quick Filter clears the active filter",
            "[tui][regression][event][filter]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    library.setFilterDraft("First");
    REQUIRE(library.applyFilter());
    REQUIRE(library.tracks().size() == 1);
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK_FALSE(fixture.shell.isInputTouched());
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));

    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft().empty());
    CHECK(library.tracks().size() == 2);
  }

  TEST_CASE("EventController - Quick Filter applies edited text after the debounce interval",
            "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("Second")));
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));
    CHECK(fixture.sleeperPtr->call(0).delay == std::chrono::milliseconds{200});
    CHECK(library.filterDraft().empty());

    REQUIRE(fixture.sleeperPtr->tryFire(0));
    REQUIRE(fixture.executor->tryDrainUntil([&library] { return library.filterDraft() == "Second"; }));
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.selectedTrackView().track->row.title == "Second");
    CHECK(fixture.shell.isInputActive());
  }

  TEST_CASE("EventController - backspacing Quick Filter to empty restores all tracks after debounce",
            "[tui][regression][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("F")));
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));
    REQUIRE(fixture.sleeperPtr->tryFire(0));
    REQUIRE(fixture.executor->tryDrainUntil([&library] { return library.filterDraft() == "F"; }));
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.selectedTrackView().track->row.title == "First");

    CHECK(controller.tryHandleEvent(ftxui::Event::Backspace));
    CHECK(fixture.shell.inputDraft().empty());
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(2));
    REQUIRE(fixture.sleeperPtr->tryFire(1));
    REQUIRE(fixture.executor->tryDrainUntil([&library] { return library.filterDraft().empty(); }));

    CHECK(library.tracks().size() == 2);
    CHECK(fixture.shell.isInputActive());
  }

  TEST_CASE("EventController - Quick Filter Enter accepts completion and cancels pending debounce",
            "[tui][regression][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    fixture.addTrack(library::test::TrackSpec{.title = "First Love", .artist = "宇多田光"});
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library, defaultKeymapPlan(), {}, completeYuduo);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("yuduo")));
    REQUIRE(fixture.shell.commandCompletion());
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft() == "\"宇多田光\"");
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.selectedTrackView().track->row.artist == "宇多田光");
    CHECK(fixture.sleeperPtr->tryWaitForCancellation(0));
    CHECK_FALSE(fixture.sleeperPtr->tryFire(0));
  }

  TEST_CASE("EventController - Quick Filter Escape applies literal text instead of the selected completion",
            "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library, defaultKeymapPlan(), {}, completeYuduo);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("yuduo")));
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));

    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft() == "yuduo");
    CHECK(fixture.sleeperPtr->tryWaitForCancellation(0));
  }

  TEST_CASE("EventController - Quick Filter Tab accepts completion and stays live", "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    fixture.addTrack(library::test::TrackSpec{.title = "First Love", .artist = "宇多田光"});
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library, defaultKeymapPlan(), {}, completeYuduo);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("yuduo")));
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));

    CHECK(controller.tryHandleEvent(ftxui::Event::Tab));
    CHECK(fixture.shell.isInputActive());
    CHECK(fixture.shell.inputDraft() == "\"宇多田光\"");
    REQUIRE(fixture.sleeperPtr->tryWaitForCancellation(0));
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(2));

    REQUIRE(fixture.sleeperPtr->tryFire(1));
    REQUIRE(fixture.executor->tryDrainUntil([&library] { return library.filterDraft() == "\"宇多田光\""; }));
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.selectedTrackView().track->row.artist == "宇多田光");
    CHECK(fixture.shell.isInputActive());
  }

  TEST_CASE("EventController - Quick Filter shows transient expression errors without posting notifications",
            "[tui][regression][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("$artist =")));
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));
    REQUIRE(fixture.sleeperPtr->tryFire(0));
    REQUIRE(fixture.executor->tryDrainUntil([&library] { return library.filterDraft() == "$artist ="; }));

    CHECK(fixture.shell.isInputActive());
    CHECK(library.filterError().contains("Filter error:"));
    CHECK(fixture.runtimePtr->notifications().feed().entries.empty());

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.shell.isInputActive());

    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Warning);
    CHECK(std::get<std::string>(feed.entries.back().message).contains("Filter error:"));
  }

  TEST_CASE("EventController - teardown cancellation retires a pending Quick Filter debounce",
            "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("First")));
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));

    controller.cancelTransientInteractions();

    CHECK(fixture.sleeperPtr->tryWaitForCancellation(0));
    CHECK(library.filterDraft().empty());
  }

  TEST_CASE("EventController - destruction cancels a pending Quick Filter debounce", "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();

    {
      auto controller = fixture.makeEvents(library);
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
      CHECK(controller.tryHandleEvent(ftxui::Event::Character("First")));
      REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));
    }

    CHECK(fixture.sleeperPtr->tryWaitForCancellation(0));
    CHECK(library.filterDraft().empty());
  }

  TEST_CASE("EventController - completion keys distinguish acceptance from submission", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller =
      fixture.makeEvents(library,
                         defaultKeymapPlan(),
                         [](std::string_view const draft, std::size_t) -> std::optional<rt::CompletionResult>
                         {
                           if (draft != "de")
                           {
                             return std::nullopt;
                           }

                           return rt::CompletionResult{
                             .replaceBegin = 0,
                             .replaceEnd = 2,
                             .items =
                               {
                                 rt::CompletionItem{.displayText = ":detail",
                                                    .insertText = "detail",
                                                    .detail = rt::CompletionDetail::makeResolvedText("track detail")},
                                 rt::CompletionItem{.displayText = ":devices",
                                                    .insertText = "devices",
                                                    .detail = rt::CompletionDetail::makeResolvedText("output devices")},
                               },
                           };
                         });

    CHECK(controller.tryHandleEvent(ftxui::Event::Character(":")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("d")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("e")));
    REQUIRE(fixture.shell.commandCompletion());

    SECTION("Tab accepts the selection and keeps the Command Palette active")
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::Tab));
      CHECK(fixture.shell.isInputActive());
      CHECK(fixture.shell.inputDraft() == "detail");
      CHECK_FALSE(fixture.shell.commandCompletion());

      CHECK(controller.tryHandleEvent(ftxui::Event::Return));
      CHECK_FALSE(fixture.shell.isInputActive());
      CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    }

    SECTION("Return activates the selected action for an incomplete command")
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::Return));
      CHECK_FALSE(fixture.shell.isInputActive());
      CHECK(library.filterDraft().empty());
      CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    }

    SECTION("Page keys move by the bounded list page")
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::PageDown));
      CHECK(fixture.shell.commandCompletionSelection() == 1);
      CHECK(controller.tryHandleEvent(ftxui::Event::PageDown));
      CHECK(fixture.shell.commandCompletionSelection() == 1);
      CHECK(controller.tryHandleEvent(ftxui::Event::PageUp));
      CHECK(fixture.shell.commandCompletionSelection() == 0);
      CHECK(fixture.shell.isInputActive());
    }

    SECTION("Arrow keys cycle through completion items")
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::ArrowDown));
      CHECK(fixture.shell.commandCompletionSelection() == 1);
      CHECK(controller.tryHandleEvent(ftxui::Event::ArrowDown));
      CHECK(fixture.shell.commandCompletionSelection() == 0);
      CHECK(controller.tryHandleEvent(ftxui::Event::ArrowUp));
      CHECK(fixture.shell.commandCompletionSelection() == 1);
      CHECK(fixture.shell.isInputActive());
    }
  }

  TEST_CASE("EventController - command input escape cancels the draft", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character(":")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("h")));
    CHECK(fixture.shell.isInputActive());
    CHECK(fixture.shell.inputDraft() == "h");

    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(fixture.shell.inputDraft().empty());
  }

  TEST_CASE("EventController - detail shortcut toggles the detail overlay", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - effective plan replaces old root keys instead of adding bypasses", "[tui][unit][keymap]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({
      {"tui.shell.toggleListChooser", {}},
      {"tui.shell.toggleTrackDetail", {"F2"}},
    });
    auto const keymapPlan = KeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, keymapPlan);

    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Character("l")));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.tryHandleEvent(ftxui::Event::F2));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(controller.tryHandleEvent(ftxui::Event::F2));
    CHECK(fixture.shell.overlay() == Overlay::None);

    enterCommand(controller, "lists");
    CHECK(fixture.shell.isNavigationFocused());
  }

  TEST_CASE("EventController - scoped protocol wins over conflicting root bindings", "[tui][unit][keymap]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({
      {"tui.shell.toggleListChooser", {"Enter"}},
      {"tui.shell.toggleTrackDetail", {"Down", "Q"}},
      {"tui.shell.quit", {"Escape"}},
    });
    auto const keymapPlan = KeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, keymapPlan);

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.tryHandleEvent(ftxui::Event::ArrowDown));
    CHECK(library.selectedTrack() == 1);
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.shell.isNavigationFocused());
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.shell.isNavigationFocused());
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

    fixture.shell.beginInput(ShellInputMode::QuickFilter);
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("q")));
    CHECK(fixture.shell.inputDraft() == "q");
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - detail follows the track table while it stays open", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);
    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.tryHandleEvent(ftxui::Event::ArrowDown));
    CHECK(library.selectedTrack() == 1);
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.tryHandleEvent(ftxui::Event::Home));
    CHECK(library.selectedTrack() == 0);

    CHECK(controller.tryHandleEvent(ftxui::Event::End));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(library.tracks().size()) - 1);

    CHECK(controller.tryHandleEvent(ftxui::Event::PageUp));
    CHECK(library.selectedTrack() == 0);
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
  }

  TEST_CASE("EventController - detail leaves workspace commands to the workspace", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{
      .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto const secondSection = library.sections()[1];
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("}")));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(secondSection.rowBegin));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("{")));
    CHECK(library.selectedTrack() == 0);

    // Text input is its own mode: it suspends the workspace without closing the
    // inspector, so Escape leaves Detail exactly as it was.
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(fixture.shell.isInputActive());
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
  }

  TEST_CASE("EventController - detail leaves transport keys reaching playback", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    REQUIRE_FALSE(library.tracks().empty());
    auto& playback = fixture.runtimePtr->playback();
    auto const trackId = library.tracks()[0].id;
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);

    // Enter starts the selection the inspector is describing.
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.tryWaitForPlayback(trackId));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character(" ")));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
  }

  TEST_CASE("EventController - closing detail ends a scrollbar drag it admitted", "[tui][regression][event][detail]")
  {
    auto fixture = EventControllerFixture{};

    for (std::int32_t index = 0; index < 20; ++index)
    {
      fixture.addTrack(library::test::TrackSpec{.title = std::format("Filler {}", index)});
    }

    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 40, .y_min = 2, .y_max = 12};
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 40, .y = 4};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    auto const draggedSelection = library.selectedTrack();

    // Escape takes the pane away, so the drag has nothing left to aim at.
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(fixture.shell.overlay() == Overlay::None);

    auto drag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 40, .y = 11};
    controller.tryHandleEvent(ftxui::Event::Mouse("", drag));
    CHECK(library.selectedTrack() == draggedSelection);
  }

  TEST_CASE("EventController - a modal overlay ends a column drag detail admitted", "[tui][regression][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20,
                              .availableColumns = 100}};
    auto& columnLayouts = fixture.trackColumnLayouts;
    auto& resizePreview = fixture.trackColumnResizePreview;
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    auto drag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 34, .y = 2};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", drag)));
    REQUIRE_FALSE(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());

    // Help replaces Detail and blocks the workspace, so the preview rolls back.
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("?")));
    REQUIRE(fixture.shell.overlay() == Overlay::Help);
    CHECK(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());

    controller.tryHandleEvent(ftxui::Event::Mouse("", drag));
    CHECK(columnLayouts.snapshot().empty());
  }

  TEST_CASE("EventController - another overlay replaces detail", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("?")));
    CHECK(fixture.shell.overlay() == Overlay::Help);

    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - detail leaves table mouse gestures available", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};

    for (std::int32_t index = 0; index < 8; ++index)
    {
      fixture.addTrack(library::test::TrackSpec{.title = std::format("Filler {}", index)});
    }

    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 40, .y_min = 2, .y_max = 12};
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);
    REQUIRE(library.selectedTrack() == 0);

    auto wheel = ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", wheel)));
    CHECK(library.selectedTrack() > 0);
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
  }

  TEST_CASE("EventController - overlay shortcuts update visible shell state", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("l")));
    CHECK(fixture.shell.isNavigationFocused());

    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("?")));
    CHECK(fixture.shell.overlay() == Overlay::Help);

    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::QualityPanel);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - output shortcut toggles the output overlay", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("o")));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("o")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - modal overlays swallow workspace shortcuts", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({{"tui.shell.toggleListChooser", {"F2"}}});
    auto const keymapPlan = KeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, keymapPlan);

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("a")));
    REQUIRE(fixture.shell.overlay() == Overlay::QualityPanel);

    auto const workspaceEvents = std::vector{
      ftxui::Event::PageDown,       ftxui::Event::End,
      ftxui::Event::Return,         ftxui::Event::Character("p"),
      ftxui::Event::Character("v"), ftxui::Event::Character("j"),
      ftxui::Event::Character("k"), ftxui::Event::Character(" "),
      ftxui::Event::Character("s"), ftxui::Event::Character("["),
      ftxui::Event::Character("]"), ftxui::Event::Character("+"),
      ftxui::Event::Character("-"), ftxui::Event::Character("="),
      ftxui::Event::Character("c"), ftxui::Event::Character("r"),
      ftxui::Event::Character("{"), ftxui::Event::Character("}"),
      ftxui::Event::CtrlL,          ftxui::Event::F2,
    };

    for (auto const& event : workspaceEvents)
    {
      CHECK(controller.tryHandleEvent(event));
      CHECK(library.selectedTrack() == 0);
      CHECK(fixture.shell.overlay() == Overlay::QualityPanel);
    }

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("a")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - notification x remains protocol-owned when there is nothing to dismiss",
            "[tui][unit][keymap]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({{"tui.shell.toggleNotifications", {"X"}}});
    auto const keymapPlan = KeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, keymapPlan);
    fixture.shell.openOverlay(Overlay::Notifications);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("x")));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);
  }

  TEST_CASE("EventController - non-list modal overlays do not page the track table", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(library.selectedTrack() == 0);

    fixture.shell.openOverlay(Overlay::OutputDevices);
    CHECK(controller.tryHandleEvent(ftxui::Event::PageDown));
    CHECK(controller.tryHandleEvent(ftxui::Event::Home));
    CHECK(controller.tryHandleEvent(ftxui::Event::End));
    CHECK(library.selectedTrack() == 0);

    fixture.shell.openOverlay(Overlay::Help);
    CHECK(controller.tryHandleEvent(ftxui::Event::PageDown));
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - presentation shortcut toggles the views overlay closed", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("p")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("p")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - presentation shortcut selects track views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("p")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    auto const albumsIndex = presentationIndex(library, "albums");
    REQUIRE(albumsIndex >= 0);
    CHECK(controller.tryHandleEvent(ftxui::Event::Home));

    for (std::int32_t index = 0; index < albumsIndex; ++index)
    {
      CHECK(controller.tryHandleEvent(ftxui::Event::ArrowDown));
    }

    CHECK(library.selectedPresentation() == albumsIndex);

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtimePtr->views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - presentation navigation keys move within views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("p")));
    CHECK(controller.tryHandleEvent(ftxui::Event::End));
    CHECK(library.selectedPresentation() == static_cast<std::int32_t>(library.presentationEntries().size()) - 1);

    CHECK(controller.tryHandleEvent(ftxui::Event::Home));
    CHECK(library.selectedPresentation() == 0);

    CHECK(controller.tryHandleEvent(ftxui::Event::PageDown));
    CHECK(library.selectedPresentation() == 10);

    CHECK(controller.tryHandleEvent(ftxui::Event::PageUp));
    CHECK(library.selectedPresentation() == 0);

    CHECK(controller.tryHandleEvent(ftxui::Event::ArrowUp));
    CHECK(library.selectedPresentation() == 0);
  }

  TEST_CASE("EventController - navigation shortcuts move the focused selection", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(library.tracks().size() == 2);
    CHECK(library.selectedTrack() == 0);

    CHECK(controller.tryHandleEvent(ftxui::Event::ArrowDown));
    CHECK(library.selectedTrack() == 1);

    CHECK(controller.tryHandleEvent(ftxui::Event::ArrowUp));
    CHECK(library.selectedTrack() == 0);

    CHECK(controller.tryHandleEvent(ftxui::Event::End));
    CHECK(library.selectedTrack() == 1);

    CHECK(controller.tryHandleEvent(ftxui::Event::Home));
    CHECK(library.selectedTrack() == 0);

    fixture.shell.focusNavigation();
    CHECK(controller.tryHandleEvent(ftxui::Event::PageDown));
    CHECK(library.navigation().selectedIndex() == 0);
    CHECK(controller.tryHandleEvent(ftxui::Event::PageUp));
    CHECK(library.navigation().selectedIndex() == 0);
  }

  TEST_CASE("EventController - commands apply filters and clear them", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    enterQuickFilter(controller, "First");
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft() == "First");
    CHECK(library.tracks().size() == 1);
    REQUIRE(library.selectedTrackView().track != nullptr);
    CHECK(library.selectedTrackView().track->row.title == "First");

    enterCommand(controller, "clear");
    CHECK(library.filterDraft().empty());
    CHECK(library.tracks().size() == 2);
  }

  TEST_CASE("EventController - filter errors preserve rows and enter the notification feed",
            "[tui][regression][event][library]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto const activeViewId = library.activeViewId();
    REQUIRE(fixture.runtimePtr->workspace().closeView(activeViewId));
    auto controller = fixture.makeEvents(library);

    enterQuickFilter(controller, "First");

    CHECK(library.filterDraft() == "First");
    CHECK(library.activeViewId() == activeViewId);
    CHECK(library.tracks().size() == 2);
    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Error);
    auto const& message = std::get<std::string>(feed.entries.back().message);
    CHECK(message.starts_with("Filter failed: View "));
    CHECK(message.ends_with(" does not exist"));
  }

  TEST_CASE("EventController - named commands route to shell playback and library actions", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    enterCommand(controller, "lists");
    CHECK(fixture.shell.isNavigationFocused());
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));

    enterCommand(controller, "detail");
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));

    enterCommand(controller, "quality");
    CHECK(fixture.shell.overlay() == Overlay::QualityPanel);
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));

    enterCommand(controller, "views");
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));

    enterCommand(controller, "close");
    CHECK(fixture.shell.overlay() == Overlay::None);

    enterCommand(controller, "help");
    CHECK(fixture.shell.overlay() == Overlay::Help);
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));

    enterCommand(controller, "current");
    CHECK(library.selectedTrack() == 0);

    enterCommand(controller, "view albums");
    CHECK(fixture.runtimePtr->views().trackListState(library.activeViewId()).presentation.id == "albums");

    enterCommand(controller, "reload");
    CHECK(library.tracks().size() == 2);

    enterCommand(controller, "play");
    REQUIRE(fixture.tryWaitForPlayback(library.tracks()[library.selectedTrack()].id));
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
    auto& outputDevices = fixture.outputDevices;
    outputDevices.refresh();
    REQUIRE(outputDevices.viewState().rows.size() > 1);
    auto const outputRow = outputDevices.viewState().rows[1];
    auto& hitRegions = fixture.hitRegions;
    hitRegions.outputDeviceButtonBox = ftxui::Box{.x_min = 4, .x_max = 9, .y_min = 0, .y_max = 0};
    hitRegions.outputDeviceRows = {
      OutputDeviceRowHitRegion{.rowIndex = 1,
                               .backendId = outputRow.backendId,
                               .deviceId = outputRow.deviceId,
                               .profileId = outputRow.profileId,
                               .box = ftxui::Box{.x_min = 2, .x_max = 30, .y_min = 3, .y_max = 3}}};
    auto controller = fixture.makeEvents(library);

    auto clickBadge = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 6, .y = 0};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickBadge)));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    auto clickOrigin = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 0, .y = 0};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", clickOrigin)));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 3};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(currentPlayback(fixture).transport.output.selectedDevice.backendId == audio::BackendId{"test_backend"});

    enterCommand(controller, "output");
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(currentPlayback(fixture).transport.output.selectedDevice.backendId == audio::BackendId{"test_backend"});
  }

  TEST_CASE("EventController - stale output row clicks keep the picker open", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto& outputDevices = fixture.outputDevices;
    outputDevices.refresh();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.outputDeviceRows = {
      OutputDeviceRowHitRegion{.rowIndex = 1,
                               .backendId = audio::BackendId{"stale_backend"},
                               .deviceId = audio::DeviceId{"stale_device"},
                               .profileId = audio::kProfileShared,
                               .box = ftxui::Box{.x_min = 2, .x_max = 30, .y_min = 3, .y_max = 3}}};
    auto controller = fixture.makeEvents(library);

    fixture.shell.openOverlay(Overlay::OutputDevices);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 3};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickRow)));

    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);
    CHECK(currentPlayback(fixture).transport.output.selectedDevice.backendId == audio::BackendId{"test_backend"});
  }

  TEST_CASE("EventController - output selector handles page and boundary navigation keys", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider(rt::test::makePipeWireOutputStatus());
    auto library = fixture.makeLibrary();
    auto& outputDevices = fixture.outputDevices;
    auto controller = fixture.makeEvents(library);

    REQUIRE(outputDevices.selectedRow() == 1);
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("o")));
    REQUIRE(fixture.shell.overlay() == Overlay::OutputDevices);

    CHECK(controller.tryHandleEvent(ftxui::Event::PageDown));
    CHECK(outputDevices.selectedRow() == 2);
    CHECK(controller.tryHandleEvent(ftxui::Event::PageUp));
    CHECK(outputDevices.selectedRow() == 1);
    CHECK(controller.tryHandleEvent(ftxui::Event::End));
    CHECK(outputDevices.selectedRow() == 2);
    CHECK(controller.tryHandleEvent(ftxui::Event::Home));
    CHECK(outputDevices.selectedRow() == 1);
  }

  TEST_CASE("EventController - hovering the soul button shows transient quality details", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    auto renderScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(1));
    auto const playbackSnapshot = currentPlayback(fixture);
    ftxui::Render(renderScreen,
                  playbackBar(ao::test::englishMessageCatalog(),
                              PlaybackBarViewState{.playbackState = &playbackSnapshot.transport,
                                                   .soulButtonBox = &hitRegions.soulButtonBox,
                                                   .terminalColumns = 80}));
    auto controller = fixture.makeEvents(library);

    REQUIRE(hitRegions.soulButtonBox.x_min == 0);
    REQUIRE(hitRegions.soulButtonBox.x_max > hitRegions.soulButtonBox.x_min);

    auto moveOverButton = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 0, .y = 0};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", moveOverButton)));
    CHECK(controller.isQualityHoverVisible());
    CHECK(fixture.shell.overlay() == Overlay::None);

    auto moveAway = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 20, .y = 0};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", moveAway)));
    CHECK_FALSE(controller.isQualityHoverVisible());

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", moveOverButton)));
    CHECK_FALSE(controller.isQualityHoverVisible());
  }

  TEST_CASE("EventController - comma opens Settings only at workspace scope", "[tui][unit][event][settings]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(",")));
    CHECK(fixture.settingsPtr->isActive());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(":")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(",")));
    CHECK(fixture.shell.inputDraft() == ",");
    CHECK_FALSE(fixture.settingsPtr->isActive());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("e")));
    REQUIRE(fixture.trackEditPtr->isActive());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(",")));
    CHECK_FALSE(fixture.settingsPtr->isActive());
  }

  TEST_CASE("EventController - Settings remains clickable with no keyboard binding", "[tui][unit][event][settings]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({{"tui.shell.openSettings", {}}});
    auto const plan = KeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, plan);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(36), ftxui::Dimension::Fixed(1));
    auto& box = fixture.hitRegions.settingsButtonBox;
    ftxui::Render(
      screen,
      statusBar(
        ao::test::englishMessageCatalog(), StatusBarViewState{.terminalColumns = 36, .settingsButtonBox = &box}, plan));
    REQUIRE(hasHitArea(box));
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Character(",")));
    CHECK_FALSE(fixture.settingsPtr->isActive());
    auto mouse =
      ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = box.x_min, .y = box.y_min};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(controller.hoveredButton() == HoveredButton::Settings);
    mouse.motion = ftxui::Mouse::Pressed;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(fixture.settingsPtr->isActive());
  }

  TEST_CASE("EventController - closing Settings does not restore stale hover", "[tui][regression][event][settings]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    fixture.hitRegions.soulButtonBox = ftxui::Box{.x_min = 0, .x_max = 2, .y_min = 0, .y_max = 0};
    auto controller = fixture.makeEvents(library);
    auto const hover = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 1, .y = 0};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", hover)));
    REQUIRE(controller.isQualityHoverVisible());

    enterCommand(controller, "settings");

    REQUIRE(fixture.settingsPtr->isActive());
    CHECK_FALSE(controller.isQualityHoverVisible());
    CHECK(controller.hoveredButton() == HoveredButton::None);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.settingsPtr->isActive());
    CHECK_FALSE(controller.isQualityHoverVisible());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", hover)));
    CHECK(controller.isQualityHoverVisible());
  }

  TEST_CASE("EventController - hovering clickable buttons updates hover target", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.outputDeviceButtonBox = ftxui::Box{.x_min = 4, .x_max = 9, .y_min = 0, .y_max = 0};
    hitRegions.libraryButtonBox = ftxui::Box{.x_min = 2, .x_max = 12, .y_min = 23, .y_max = 23};
    hitRegions.presentationButtonBox = ftxui::Box{.x_min = 15, .x_max = 24, .y_min = 23, .y_max = 23};
    hitRegions.activityStatusBox = ftxui::Box{.x_min = 28, .x_max = 48, .y_min = 23, .y_max = 23};
    auto controller = fixture.makeEvents(library);

    auto moveOutput = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 5, .y = 0};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", moveOutput)));
    CHECK(controller.hoveredButton() == HoveredButton::OutputDevice);

    auto moveLibrary = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 3, .y = 23};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", moveLibrary)));
    CHECK(controller.hoveredButton() == HoveredButton::Library);

    auto movePresentation = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 16, .y = 23};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", movePresentation)));
    CHECK(controller.hoveredButton() == HoveredButton::Presentation);

    auto moveActivity = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 23};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", moveActivity)));
    CHECK(controller.hoveredButton() == HoveredButton::ActivityStatus);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", moveActivity)));
    CHECK(controller.hoveredButton() == HoveredButton::None);
  }

  TEST_CASE("EventController - clicking the soul button toggles playback", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    auto renderScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(1));
    auto const playbackSnapshot = currentPlayback(fixture);
    ftxui::Render(renderScreen,
                  playbackBar(ao::test::englishMessageCatalog(),
                              PlaybackBarViewState{.playbackState = &playbackSnapshot.transport,
                                                   .soulButtonBox = &hitRegions.soulButtonBox,
                                                   .terminalColumns = 80}));
    auto controller = fixture.makeEvents(library);

    REQUIRE(hitRegions.soulButtonBox.x_min < hitRegions.soulButtonBox.x_max);

    auto clickSoulButton = ftxui::Mouse{.button = ftxui::Mouse::Left,
                                        .motion = ftxui::Mouse::Pressed,
                                        .x = hitRegions.soulButtonBox.x_min,
                                        .y = hitRegions.soulButtonBox.y_min};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    REQUIRE(fixture.tryWaitForPlayback(library.tracks()[library.selectedTrack()].id));
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Playing);
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);

    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Paused);

    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Playing);
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[library.selectedTrack()].id);
  }

  TEST_CASE("EventController - unavailable transport command is gated and reported", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character(" ")));

    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Idle);
    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Warning);
    CHECK(std::get<std::string>(feed.entries.back().message) == "Playback control unavailable");
  }

  TEST_CASE("EventController - idle stop is a silent no-op", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("s")));

    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Idle);
    CHECK(fixture.runtimePtr->notifications().feed().entries.empty());
  }

  TEST_CASE("EventController - space pauses playback while output selection is pending", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    REQUIRE_FALSE(library.tracks().empty());
    auto& playback = fixture.runtimePtr->playback();
    auto& commands = playback.commands();
    auto const trackId = library.tracks()[0].id;
    REQUIRE(commands.startFromView(library.activeViewId(), trackId));
    REQUIRE(fixture.tryWaitForPlayback(trackId));
    auto controller = fixture.makeEvents(library);
    auto const selected = playback.snapshot().transport.output.selectedDevice;

    commands.setOutputDevice(selected.backendId, audio::DeviceId{"pending-device"}, selected.profileId);

    auto transport = playback.snapshot().transport;
    REQUIRE_FALSE(transport.ready);
    REQUIRE(transport.transport == audio::Transport::Playing);
    auto const notificationCount = fixture.runtimePtr->notifications().feed().entries.size();

    CHECK(controller.tryHandleEvent(ftxui::Event::Character(" ")));

    transport = playback.snapshot().transport;
    CHECK(transport.transport == audio::Transport::Paused);
    CHECK(fixture.runtimePtr->notifications().feed().entries.size() == notificationCount);
  }

  TEST_CASE("EventController - presentation mouse clicks open and select views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    auto const albumsIndex = presentationIndex(library, "albums");
    REQUIRE(albumsIndex >= 0);
    hitRegions.presentationButtonBox = ftxui::Box{.x_min = 20, .x_max = 29, .y_min = 23, .y_max = 23};
    hitRegions.presentationRows = {PresentationRowHitRegion{
      .rowIndex = albumsIndex, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = fixture.makeEvents(library);

    auto clickView = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 24, .y = 23};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickView)));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 12};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtimePtr->views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - notification shortcut opens available activity details", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& activityStatusViewModel = fixture.activityStatusViewModel;
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);

    fixture.runtimePtr->notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::pinned());

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);
    REQUIRE(activityStatusViewModel.viewState().compact.dismissible);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("x")));
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Idle);
    CHECK(fixture.shell.overlay() == Overlay::Notifications);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - panel actions use transient activity notifications when available", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& activityStatusViewModel = fixture.activityStatusViewModel;
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("d")));

    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Info);
    CHECK(activityStatusViewModel.viewState().compact.text == "Detail panel");
    CHECK_FALSE(activityStatusViewModel.viewState().compact.optAutoDismissTimeout);
    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().lifetime == rt::NotificationLifetime::transient());
  }

  TEST_CASE("EventController - root Escape is a silent no-op without an overlay", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(fixture.shell.overlay() == Overlay::None);
    REQUIRE(fixture.runtimePtr->notifications().feed().entries.empty());

    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));

    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtimePtr->notifications().feed().entries.empty());
  }

  TEST_CASE("EventController - notification mouse targets hide only activity presentation", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& activityStatusViewModel = fixture.activityStatusViewModel;
    fixture.runtimePtr->notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::history());
    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    auto const notificationId = feed.entries.front().id;
    auto& hitRegions = fixture.hitRegions;
    hitRegions.activityStatusBox = ftxui::Box{.x_min = 0, .x_max = 24, .y_min = 23, .y_max = 23};
    hitRegions.notificationDetailRows = {NotificationDetailRowHitRegion{
      .id = notificationId, .dismissible = true, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = fixture.makeEvents(library);

    auto clickActivity = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 8, .y = 23};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickActivity)));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 12};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(activityStatusViewModel.viewState().detail.items.empty());
    // Hiding the detail row retires the presentation only; the notification
    // itself stays in the feed, alongside whatever the overlay itself posted.
    auto const entries = fixture.runtimePtr->notifications().feed().entries;
    CHECK(std::ranges::any_of(entries, [notificationId](auto const& entry) { return entry.id == notificationId; }));
  }

  TEST_CASE("EventController - column resize previews then commits once on release", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20,
                              .availableColumns = 100}};
    auto& columnLayouts = fixture.trackColumnLayouts;
    auto& resizePreview = fixture.trackColumnResizePreview;
    auto changedLists = std::vector<ListId>{};
    auto changedSub =
      columnLayouts.signalChanged().connect([&](ListId const listId) noexcept { changedLists.push_back(listId); });
    auto controller = fixture.makeEvents(library);

    auto pressEdge = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", pressEdge)));

    auto moveRight = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 25, .y = 2};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", moveRight)));
    CHECK(resizePreview.listId == rt::kAllTracksListId);
    REQUIRE_FALSE(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());
    CHECK(changedLists.empty());

    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 25, .y = 2};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", release)));
    CHECK(resizePreview.layout.empty());
    REQUIRE(columnLayouts.snapshot().contains(rt::kAllTracksListId));
    REQUIRE(changedLists.size() == 1);
    CHECK(changedLists[0] == rt::kAllTracksListId);
    auto const committed = projectTerminalTrackColumnLayout(
      library.activePresentation(), columnLayouts.layoutForList(rt::kAllTracksListId), 100);
    auto const title = std::ranges::find(committed.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    REQUIRE(title != committed.columns.end());
    CHECK(title->columns == 25);

    auto moveAfterRelease = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 2};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", moveAfterRelease)));
    CHECK(changedLists.size() == 1);
  }

  TEST_CASE("EventController - column resize follows a terminal resize during the drag", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20,
                              .availableColumns = 100}};
    auto& columnLayouts = fixture.trackColumnLayouts;
    auto& resizePreview = fixture.trackColumnResizePreview;
    auto controller = fixture.makeEvents(library);

    auto const press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));

    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 26, .y_min = 2, .y_max = 2},
                              .columns = 26,
                              .availableColumns = 130}};
    auto const move = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 25, .y = 2};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", move)));
    auto const preview = projectTerminalTrackColumnLayout(
      library.activePresentation(), resizePreview.layout, hitRegions.trackColumnResizeHandles.front().availableColumns);
    auto const previewTitle = std::ranges::find(preview.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    REQUIRE(previewTitle != preview.columns.end());
    CHECK(previewTitle->columns == 25);

    auto const release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 25, .y = 2};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", release)));
    auto const committed = projectTerminalTrackColumnLayout(
      library.activePresentation(), columnLayouts.layoutForList(rt::kAllTracksListId), 130);
    auto const committedTitle =
      std::ranges::find(committed.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    REQUIRE(committedTitle != committed.columns.end());
    CHECK(committedTitle->columns == 25);
  }

  TEST_CASE("EventController - interrupted column drag does not swallow the next press", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20,
                              .availableColumns = 100}};
    auto& columnLayouts = fixture.trackColumnLayouts;
    auto& resizePreview = fixture.trackColumnResizePreview;
    auto controller = fixture.makeEvents(library);

    auto pressEdge = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", pressEdge)));
    auto firstMove = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 2};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", firstMove)));
    REQUIRE_FALSE(resizePreview.layout.empty());

    auto secondPress = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 2, .y = 8};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", secondPress)));
    CHECK(resizePreview.layout.empty());

    auto staleMove = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 40, .y = 2};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", staleMove)));
    CHECK(columnLayouts.snapshot().empty());
  }

  TEST_CASE("EventController - list navigation rolls back an in-flight column preview", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto const otherListId = fixture.addList("Other");
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20,
                              .availableColumns = 100}};
    auto& columnLayouts = fixture.trackColumnLayouts;
    auto& resizePreview = fixture.trackColumnResizePreview;
    auto controller = fixture.makeEvents(library);
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    auto move = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 28, .y = 2};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", move)));
    REQUIRE_FALSE(resizePreview.layout.empty());

    openList(library, otherListId);
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 28, .y = 2};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", release)));

    CHECK(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());
  }

  TEST_CASE("EventController - committed column layouts remain scoped to their list", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto const otherListId = fixture.addList("Other");
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20,
                              .availableColumns = 100}};
    auto& columnLayouts = fixture.trackColumnLayouts;
    auto controller = fixture.makeEvents(library);
    auto resizeCurrentList = [&](std::int32_t const releaseX)
    {
      hitRegions.trackTableRevision = library.trackRowsRevision();
      auto const press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
      auto const release =
        ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = releaseX, .y = 2};
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", release)));
    };

    auto const allTracksPresentation = library.activePresentation();
    resizeCurrentList(25);
    openList(library, otherListId);
    CHECK(columnLayouts.layoutForList(otherListId).empty());
    resizeCurrentList(30);

    REQUIRE(columnLayouts.snapshot().size() == 2);
    auto const allTracks =
      projectTerminalTrackColumnLayout(allTracksPresentation, columnLayouts.layoutForList(rt::kAllTracksListId), 100);
    auto const other =
      projectTerminalTrackColumnLayout(library.activePresentation(), columnLayouts.layoutForList(otherListId), 100);
    auto const allTracksTitle =
      std::ranges::find(allTracks.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    auto const otherTitle = std::ranges::find(other.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    REQUIRE(allTracksTitle != allTracks.columns.end());
    REQUIRE(otherTitle != other.columns.end());
    CHECK(allTracksTitle->columns == 25);
    CHECK(otherTitle->columns == 30);
  }

  TEST_CASE("EventController - teardown rolls back an in-flight column preview", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20,
                              .availableColumns = 100}};
    auto& columnLayouts = fixture.trackColumnLayouts;
    auto& resizePreview = fixture.trackColumnResizePreview;
    auto controller = fixture.makeEvents(library);
    auto const press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    auto const move = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 28, .y = 2};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", move)));
    REQUIRE_FALSE(resizePreview.layout.empty());

    controller.cancelTransientInteractions();

    CHECK(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());
  }

  TEST_CASE("EventController - mouse preference changes apply to the running controller",
            "[tui][unit][event][settings]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{.title = "Third"});
    fixture.addTrack(library::test::TrackSpec{.title = "Fourth"});
    auto library = fixture.makeLibrary();
    fixture.hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controller = fixture.makeEvents(library);
    auto const wheel =
      ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    fixture.preferences.wheelStep = 2;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", wheel)));
    CHECK(library.selectedTrack() == 2);
    fixture.preferences.mouseEnabled = false;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", wheel)));
    CHECK(library.selectedTrack() == 2);
    fixture.preferences.mouseEnabled = true;
    fixture.preferences.wheelStep = 1;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", wheel)));
    CHECK(library.selectedTrack() == 3);
  }

  TEST_CASE("EventController - mouse wheel scrolls the track table", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controller = fixture.makeEvents(library);

    REQUIRE(library.selectedTrack() == 0);

    auto wheelDown = ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", wheelDown)));
    CHECK(library.selectedTrack() == 1);

    auto wheelUp = ftxui::Mouse{.button = ftxui::Mouse::WheelUp, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", wheelUp)));
    CHECK(library.selectedTrack() == 0);

    auto wheelOutside =
      ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 23};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", wheelOutside)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - mouse drag scrolls the track table scrollbar", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controller = fixture.makeEvents(library);

    REQUIRE(library.selectedTrack() == 0);

    auto pressBottom = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 22};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", pressBottom)));
    CHECK(library.selectedTrack() == 1);

    auto dragTop = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 79, .y = 2};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", dragTop)));
    CHECK(library.selectedTrack() == 0);

    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 79, .y = 2};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", release)));
    CHECK(library.selectedTrack() == 0);

    auto dragAfterRelease = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 79, .y = 22};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", dragAfterRelease)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - scrollbar handles one-row and interrupted drags", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 2};
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::End));
    REQUIRE(library.selectedTrack() == 1);

    auto pressOnlyBodyRow =
      ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 2};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", pressOnlyBodyRow)));
    CHECK(library.selectedTrack() == 0);

    auto secondPress = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 5, .y = 2};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", secondPress)));

    auto staleDrag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 79, .y = 2};
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", staleDrag)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - scrollbar ignores empty track tables", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    enterQuickFilter(controller, "missing");
    REQUIRE(library.tracks().empty());

    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controllerWithTable = fixture.makeEvents(library);

    auto pressScrollbar = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 22};
    CHECK_FALSE(controllerWithTable.tryHandleEvent(ftxui::Event::Mouse("", pressScrollbar)));
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
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("}")));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(expected.rowBegin));

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("{")));
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
    auto controller = fixture.makeEvents(library);

    fixture.shell.focusNavigation();
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("}")));
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
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackSectionRows = {
      TrackSectionRowHitRegion{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto controller = fixture.makeEvents(library);

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(expected.rowBegin));
  }

  TEST_CASE("EventController - stale section header clicks report unavailable sections", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("songs") == "View: songs");
    REQUIRE(library.sections().empty());
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackSectionRows = {
      TrackSectionRowHitRegion{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto controller = fixture.makeEvents(library);

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - same-sized regrouping rejects a previously painted section header",
            "[tui][regression][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addTrack(
      library::test::TrackSpec{.title = "One", .artist = "Zulu", .album = "Alpha", .albumArtist = "Zulu"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "Two", .artist = "Alpha", .album = "Zulu", .albumArtist = "Alpha"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    auto controller = fixture.makeEvents(library);
    auto const count = library.sections().size();
    REQUIRE(count >= 2);
    auto const previousSection = library.sections()[1];
    auto const rendered = renderElement(trackTableView(ao::test::englishMessageCatalog(),
                                                       library.tracks(),
                                                       library.sections(),
                                                       0,
                                                       kInvalidTrackId,
                                                       library.activePresentation(),
                                                       {.sectionRowHitRegions = &fixture.hitRegions.trackSectionRows}),
                                        80,
                                        24);
    auto const& box = fixture.hitRegions.trackSectionRows[1].box;
    REQUIRE_FALSE(box.IsEmpty());
    REQUIRE(library.setPresentation("artists") == "View: artists");
    REQUIRE(library.sections().size() == count);
    auto const& currentSection = library.sections()[1];
    REQUIRE((currentSection.primaryText != previousSection.primaryText ||
             currentSection.secondaryText != previousSection.secondaryText));
    library.setSelectedTrackIndex(0);

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = box.x_min + 1, .y = box.y_min})));
    CHECK(library.selectedTrack() == 0);
    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(std::get<std::string>(feed.entries.back().message) == "Section is no longer available");
  }

  TEST_CASE("EventController - refreshed presentation rejects stale column resize gestures",
            "[tui][regression][mouse][event]")
  {
    for (bool const refreshBeforePress : {true, false})
    {
      auto fixture = EventControllerFixture{};
      auto library = fixture.makeLibrary();
      auto controller = fixture.makeEvents(library);
      fixture.hitRegions.trackColumnResizeHandles = {{.field = rt::TrackField::Title,
                                                      .box = {.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                                                      .columns = 20,
                                                      .availableColumns = 100}};

      if (refreshBeforePress)
      {
        REQUIRE(library.setPresentation("albums") == "View: albums");
      }

      controller.tryHandleEvent(
        ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2}));

      if (!refreshBeforePress)
      {
        REQUIRE(library.setPresentation("albums") == "View: albums");
      }

      controller.tryHandleEvent(
        ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 30, .y = 2}));
      CHECK(fixture.trackColumnLayouts.snapshot().empty());
      CHECK(fixture.trackColumnResizePreview.listId == kInvalidListId);
    }
  }

  TEST_CASE("EventController - list chooser return opens the selected list", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    fixture.shell.focusNavigation();

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(library.currentListTitle() == "All Tracks");
    auto const feed = fixture.runtimePtr->notifications().feed();
    CHECK(feed.entries.empty());
    CHECK_FALSE(fixture.shell.isNavigationFocused());
  }

  TEST_CASE("EventController - playback shortcuts update controls", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto controller = fixture.makeEvents(library);
    auto& playback = fixture.runtimePtr->playback();
    auto& commands = playback.commands();

    commands.setVolume(0.50F);
    commands.setMuted(true);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("[")));
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{0});
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("]")));
    auto transport = playback.snapshot().transport;
    CHECK(transport.elapsed == transport.duration);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("-")));
    transport = playback.snapshot().transport;
    CHECK(transport.volume.level < 0.50F);
    CHECK(transport.volume.muted);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("+")));
    transport = playback.snapshot().transport;
    CHECK(transport.volume.level > 0.49F);
    CHECK_FALSE(transport.volume.muted);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("s")));
    CHECK(playback.snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("EventController - relative seek is inert without a known duration", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    auto& playback = fixture.runtimePtr->playback();
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot) noexcept
                                                    { snapshots.push_back(snapshot); });

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("[")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("]")));

    CHECK(snapshots.empty());
    CHECK(playback.snapshot().transport.duration == std::chrono::milliseconds{0});
  }

  TEST_CASE("EventController - mouse click on seek rail previews then commits the target position",
            "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& playback = fixture.runtimePtr->playback();
    auto const duration = playback.snapshot().transport.duration;

    auto& hitRegions = fixture.hitRegions;
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](std::chrono::milliseconds const elapsed) noexcept
                                                      { seekPreviews.push_back(elapsed); });
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot) noexcept
                                                    { snapshots.push_back(snapshot); });
    auto controller = fixture.makeEvents(library);

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 20, .y = 1};

    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", release)));

    REQUIRE(seekPreviews.size() == 1);
    CHECK(seekPreviews[0] == duration / 2);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].transport.elapsed == duration / 2);
  }

  TEST_CASE("EventController - mouse drag on seek rail clamps release outside the rail", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& playback = fixture.runtimePtr->playback();
    auto const duration = playback.snapshot().transport.duration;

    auto& hitRegions = fixture.hitRegions;
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](std::chrono::milliseconds const elapsed) noexcept
                                                      { seekPreviews.push_back(elapsed); });
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot) noexcept
                                                    { snapshots.push_back(snapshot); });
    auto controller = fixture.makeEvents(library);

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 1};
    auto dragOutside = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 99, .y = 1};
    auto releaseOutside = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 99, .y = 1};

    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", dragOutside)));
    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", releaseOutside)));

    REQUIRE(seekPreviews.size() == 2);
    CHECK(seekPreviews[0] == std::chrono::milliseconds{0});
    CHECK(seekPreviews[1] == duration);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].transport.elapsed == duration);
  }

  TEST_CASE("EventController - seek drag releases over the docked Lists pane", "[tui][regression][mouse][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& playback = fixture.runtimePtr->playback();
    playback.commands().pause();
    auto const before = playback.snapshot().transport;
    fixture.hitRegions.navigationLayout = navigationGeometry(120, 0, true, false, false);
    fixture.hitRegions.navigation.panel.box = {.x_min = 0, .x_max = 25, .y_min = 2, .y_max = 15};
    fixture.hitRegions.seekRailBox = {.x_min = 30, .x_max = 50, .y_min = 18, .y_max = 18};
    auto previews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = playback.events().onSeekPreview([&previews](std::chrono::milliseconds const elapsed) noexcept
                                                      { previews.push_back(elapsed); });
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 50, .y = 18})));
    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 5, .y = 5})));
    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 5, .y = 5})));

    CHECK(previews.size() == 2);
    CHECK(previews.back() == std::chrono::milliseconds{0});
    auto const released = playback.snapshot().transport;
    CHECK(released.finalSeekRevision.value == before.finalSeekRevision.value + 1);
    CHECK(released.elapsed == std::chrono::milliseconds{0});
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 50, .y = 18}));
    CHECK(previews.size() == 2);
    CHECK(playback.snapshot().transport.finalSeekRevision == released.finalSeekRevision);
  }

  TEST_CASE("EventController - teardown cancellation stabilizes an active seek drag",
            "[tui][regression][event][lifecycle]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& playback = fixture.runtimePtr->playback();
    playback.commands().pause();
    auto const before = playback.snapshot().transport;
    auto& hitRegions = fixture.hitRegions;
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](std::chrono::milliseconds const elapsed) noexcept
                                                      { seekPreviews.push_back(elapsed); });
    auto controller = fixture.makeEvents(library);
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 30, .y = 1};
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 30, .y = 1};

    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(seekPreviews.size() == 1);
    CHECK(seekPreviews[0] == before.duration);
    CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);

    controller.cancelTransientInteractions();

    auto const stabilized = playback.snapshot().transport;
    CHECK(stabilized.elapsed == before.elapsed);
    CHECK(stabilized.finalSeekRevision.value == before.finalSeekRevision.value + 1);
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", release)));
    CHECK(playback.snapshot().transport.finalSeekRevision == stabilized.finalSeekRevision);
  }

  TEST_CASE("EventController - disabled seek rail ignores mouse clicks", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& hitRegions = fixture.hitRegions;
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = fixture.runtimePtr->playback().events().onSeekPreview(
      [&seekPreviews](std::chrono::milliseconds const elapsed) noexcept { seekPreviews.push_back(elapsed); });
    auto controller = fixture.makeEvents(library);

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekPreviews.empty());
  }

  TEST_CASE("EventController - modal overlays block seek rail mouse clicks", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& hitRegions = fixture.hitRegions;
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = fixture.runtimePtr->playback().events().onSeekPreview(
      [&seekPreviews](std::chrono::milliseconds const elapsed) noexcept { seekPreviews.push_back(elapsed); });
    auto controller = fixture.makeEvents(library);

    fixture.shell.openOverlay(Overlay::Help);
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekPreviews.empty());
  }

  TEST_CASE("EventController - text input blocks seek rail mouse clicks", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& hitRegions = fixture.hitRegions;
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = fixture.runtimePtr->playback().events().onSeekPreview(
      [&seekPreviews](std::chrono::milliseconds const elapsed) noexcept { seekPreviews.push_back(elapsed); });
    auto controller = fixture.makeEvents(library);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    CHECK(seekPreviews.empty());
  }

  TEST_CASE("EventController - text input blocks workspace mouse controls", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    fixture.addTrack(library::test::TrackSpec{
      .title = "Grouped", .artist = "Artist", .album = "Grouped Album", .albumArtist = "Artist"});
    auto library = fixture.makeLibrary();
    REQUIRE(library.setPresentation("albums") == "View: albums");
    REQUIRE(library.sections().size() >= 2);
    auto& hitRegions = fixture.hitRegions;
    hitRegions.outputDeviceButtonBox = ftxui::Box{.x_min = 4, .x_max = 9, .y_min = 0, .y_max = 0};
    hitRegions.soulButtonBox = ftxui::Box{.x_min = 0, .x_max = 2, .y_min = 0, .y_max = 0};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20,
                              .availableColumns = 100}};
    hitRegions.trackSectionRows = {
      TrackSectionRowHitRegion{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto& columnLayouts = fixture.trackColumnLayouts;
    auto& resizePreview = fixture.trackColumnResizePreview;
    auto controller = fixture.makeEvents(library);

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(fixture.shell.isInputActive());

    auto clickSoul = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 1, .y = 0};
    auto clickOutput = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 6, .y = 0};
    auto wheelDown = ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    auto pressResize = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    auto dragResize = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 2};
    auto pressScrollbar = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 79, .y = 22};
    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};

    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", clickSoul)));
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", clickOutput)));
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", wheelDown)));
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", pressResize)));
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", dragResize)));
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", pressScrollbar)));
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", clickSection)));

    CHECK(fixture.shell.isInputActive());
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(library.selectedTrack() == 0);
    CHECK(columnLayouts.snapshot().empty());
    CHECK(resizePreview.layout.empty());
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Idle);
  }

  TEST_CASE("EventController - modal overlays cancel active seek rail drags", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto& hitRegions = fixture.hitRegions;
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto& playback = fixture.runtimePtr->playback();
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](std::chrono::milliseconds const elapsed) noexcept
                                                      { seekPreviews.push_back(elapsed); });
    auto controller = fixture.makeEvents(library);

    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 1};
    auto drag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 1};
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 30, .y = 1};

    CHECK(controller.tryHandleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(seekPreviews.size() == 1);
    CHECK(seekPreviews[0] == std::chrono::milliseconds{0});

    fixture.shell.openOverlay(Overlay::Help);
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", drag)));
    CHECK(seekPreviews.size() == 1);
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{0});

    fixture.shell.closeOverlay();
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Mouse("", release)));
    CHECK(seekPreviews.size() == 1);
  }

  TEST_CASE("EventController - current track shortcut reveals playback selection", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.tryHandleEvent(ftxui::Event::ArrowDown));
    REQUIRE(library.selectedTrack() == 1);
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.tryWaitForPlayback(library.tracks()[1].id));
    REQUIRE(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[1].id);

    CHECK(controller.tryHandleEvent(ftxui::Event::Home));
    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.tryHandleEvent(ftxui::Event::Character("c")));
    fixture.executor->drain();
    CHECK(library.selectedTrack() == 1);
  }

  TEST_CASE("EventController - track mouse selection supports modifiers and rejects stale identities",
            "[tui][unit][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    auto const first = library.tracks()[0].id;
    auto const second = library.tracks()[1].id;
    fixture.hitRegions.trackRows = {
      TrackRowHitRegion{.id = first, .rowIndex = 0, .box = {.x_min = 3, .x_max = 40, .y_min = 4, .y_max = 4}},
      TrackRowHitRegion{.id = second, .rowIndex = 1, .box = {.x_min = 3, .x_max = 40, .y_min = 5, .y_max = 5}}};
    auto mouse =
      ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .control = true, .x = 10, .y = 5};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(library.selectedTrack() == 1);
    CHECK(library.markedIds().contains(second));
    mouse.control = false;
    mouse.shift = true;
    mouse.y = 4;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(library.markedIds().size() == 2);
    mouse.shift = false;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(library.markedIds().empty());
    CHECK(library.selectedTrack() == 0);
    fixture.hitRegions.trackRows[1].id = kInvalidTrackId;
    mouse.y = 5;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(library.selectedTrack() == 0);
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("EventController - a double click plays the clicked track while an intervening key breaks the pair",
            "[tui][unit][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    auto const target = library.tracks()[1].id;
    fixture.hitRegions.trackRows = {
      TrackRowHitRegion{.id = target, .rowIndex = 1, .box = {.x_min = 3, .x_max = 40, .y_min = 5, .y_max = 5}}};
    auto const click = ftxui::Event::Mouse(
      "", ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5});
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == kInvalidTrackId);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::ArrowUp));
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == kInvalidTrackId);
    REQUIRE(controller.tryHandleEvent(click));
    REQUIRE(fixture.tryWaitForPlayback(target));
  }

  TEST_CASE("EventController - rendered Library rows open the matching saved list", "[tui][unit][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto const listId = fixture.addList("Mouse List");
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.focusNavigation();
    fixture.hitRegions.navigationLayout = navigationGeometry(80, 0, true, true, false);
    auto const rendered =
      renderElement(navigationPanel(ao::test::englishMessageCatalog(),
                                    library.navigation(),
                                    library.currentListId(),
                                    defaultKeymapPlan(),
                                    {.columns = 26, .focused = true, .regions = &fixture.hitRegions.navigation}),
                    26,
                    15);
    auto const optBox = findTextCells(rendered.screen, "Mouse List");
    REQUIRE(optBox);
    auto const click = ftxui::Event::Mouse(
      "",
      ftxui::Mouse{
        .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optBox->x_min, .y = optBox->y_min});
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(library.currentListId() == listId);
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - visible status shortcuts open their matching surface", "[tui][unit][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    auto const rendered =
      renderElement(statusBar(ao::test::englishMessageCatalog(),
                              StatusBarViewState{.terminalColumns = 120,
                                                 .shell = &fixture.shell,
                                                 .actionHitRegions = &fixture.hitRegions.statusActions},
                              defaultKeymapPlan()),
                    120,
                    1);
    auto const optBox = findTextCells(rendered.screen, "lists");
    REQUIRE(optBox);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse(
      "",
      ftxui::Mouse{
        .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optBox->x_min, .y = optBox->y_min})));
    CHECK(fixture.shell.isNavigationFocused());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    auto const optHelp = findTextCells(rendered.screen, "help");
    REQUIRE(optHelp);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse(
      "",
      ftxui::Mouse{
        .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optHelp->x_min, .y = optHelp->y_min})));
    CHECK(fixture.shell.overlay() == Overlay::Help);
  }

  TEST_CASE("EventController - command candidate clicks execute through the command protocol",
            "[tui][unit][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.beginInput(ShellInputMode::Command, "set");
    fixture.shell.setCommandCompletion(
      rt::CompletionResult{.replaceBegin = 0,
                           .replaceEnd = 3,
                           .items = {rt::CompletionItem{.displayText = "settings", .insertText = "settings"}}});
    auto const rendered = renderElement(
      mousePanel(
        commandPalettePanel(
          ao::test::englishMessageCatalog(), fixture.shell, defaultKeymapPlan(), 60, &fixture.hitRegions.completion),
        fixture.hitRegions.inputPanel),
      60,
      15);
    REQUIRE(fixture.hitRegions.completion.rows.size() == 1);
    auto const box = fixture.hitRegions.completion.rows.front();
    CHECK_FALSE(box.IsEmpty());
    CHECK(rendered.text.contains("settings"));
    auto const click = ftxui::Event::Mouse(
      "", ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = box.x_min, .y = box.y_min});
    REQUIRE(controller.tryHandleEvent(click));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(fixture.settingsPtr->isActive());
  }

  TEST_CASE("EventController - completion mouse coordinates cannot accept an older draft",
            "[tui][regression][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.beginInput(ShellInputMode::QuickFilter, "old");
    fixture.shell.setCommandCompletion(
      rt::CompletionResult{.replaceBegin = 0,
                           .replaceEnd = 3,
                           .items = {rt::CompletionItem{.displayText = "older", .insertText = "older"}}});
    auto const rendered = renderElement(mousePanel(quickFilterCompletionPanel(ao::test::englishMessageCatalog(),
                                                                              fixture.shell,
                                                                              defaultKeymapPlan(),
                                                                              60,
                                                                              {},
                                                                              &fixture.hitRegions.completion),
                                                   fixture.hitRegions.inputPanel),
                                        60,
                                        15);
    auto const optBox = findTextCells(rendered.screen, "older");
    REQUIRE(optBox);
    fixture.shell.insertInputText("x");
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse(
      "",
      ftxui::Mouse{
        .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optBox->x_min, .y = optBox->y_min})));
    CHECK(fixture.shell.inputDraft() == "oldx");
  }

  TEST_CASE("EventController - painted mode controls toggle shuffle and cycle every repeat mode",
            "[tui][unit][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto controller = fixture.makeEvents(library);
    auto const rendered = renderElement(playbackBar(library.textCatalog(),
                                                    {.shuffleBox = &fixture.hitRegions.shuffleBox,
                                                     .repeatBox = &fixture.hitRegions.repeatBox,
                                                     .terminalColumns = 80}),
                                        80,
                                        1);
    REQUIRE(findTextCells(rendered.screen, "⇄"));
    REQUIRE(findTextCells(rendered.screen, "↻"));
    auto mouse = ftxui::Mouse{.button = ftxui::Mouse::Left,
                              .motion = ftxui::Mouse::Pressed,
                              .x = fixture.hitRegions.shuffleBox.x_min,
                              .y = fixture.hitRegions.shuffleBox.y_min};

    for (auto const shuffle : {rt::ShuffleMode::On, rt::ShuffleMode::Off, rt::ShuffleMode::On})
    {
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
      fixture.executor->drain();
      CHECK(currentPlayback(fixture).succession.shuffle == shuffle);
      CHECK(currentPlayback(fixture).succession.repeat == rt::RepeatMode::Off);
    }

    mouse.x = fixture.hitRegions.repeatBox.x_min;
    mouse.y = fixture.hitRegions.repeatBox.y_min;

    for (auto const repeat : {rt::RepeatMode::All, rt::RepeatMode::One, rt::RepeatMode::Off})
    {
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
      fixture.executor->drain();
      CHECK(currentPlayback(fixture).succession.repeat == repeat);
      CHECK(currentPlayback(fixture).succession.shuffle == rt::ShuffleMode::On);
      CHECK(library.selectedTrack() == 0);
    }
  }

  TEST_CASE("EventController - mode clicks respect mouse preferences and foreground ownership",
            "[tui][unit][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.shuffleBox = {.x_min = 40, .x_max = 40, .y_min = 0, .y_max = 0};
    fixture.hitRegions.repeatBox = {.x_min = 42, .x_max = 43, .y_min = 0, .y_max = 0};

    auto const checkModesUnchanged = [&fixture]
    {
      fixture.executor->drain();
      CHECK(currentPlayback(fixture).succession.shuffle == rt::ShuffleMode::Off);
      CHECK(currentPlayback(fixture).succession.repeat == rt::RepeatMode::Off);
    };

    for (auto const column : {40, 42})
    {
      auto mouse = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = column, .y = 0};
      fixture.preferences.mouseEnabled = false;
      controller.tryHandleEvent(ftxui::Event::Mouse("", mouse));
      checkModesUnchanged();
      fixture.preferences.mouseEnabled = true;
      mouse.button = ftxui::Mouse::Right;
      controller.tryHandleEvent(ftxui::Event::Mouse("", mouse));
      checkModesUnchanged();
      mouse.button = ftxui::Mouse::Left;
      mouse.motion = ftxui::Mouse::Released;
      controller.tryHandleEvent(ftxui::Event::Mouse("", mouse));
      checkModesUnchanged();
      mouse.motion = ftxui::Mouse::Moved;
      controller.tryHandleEvent(ftxui::Event::Mouse("", mouse));
      checkModesUnchanged();
      mouse.motion = ftxui::Mouse::Pressed;

      fixture.shell.openOverlay(Overlay::DetailPanel);
      fixture.hitRegions.overlayPanel.box = {.x_min = 0, .x_max = 79, .y_min = 0, .y_max = 10};
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
      checkModesUnchanged();
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
      fixture.shell.openOverlay(Overlay::Help);
      fixture.hitRegions.overlayPanel.box = {.x_min = 10, .x_max = 60, .y_min = 2, .y_max = 10};
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
      CHECK(fixture.shell.overlay() == Overlay::None);
      checkModesUnchanged();
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(":")));
      fixture.hitRegions.inputPanel.box = {.x_min = 10, .x_max = 60, .y_min = 2, .y_max = 10};
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
      CHECK_FALSE(fixture.shell.isInputActive());
      checkModesUnchanged();
    }
  }

  TEST_CASE("EventController - volume mouse wheel and mute share the volume model", "[tui][unit][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.volumeBox = {.x_min = 70, .x_max = 79, .y_min = 0, .y_max = 0};
    auto mouse = ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 75, .y = 0};
    auto const before = currentPlayback(fixture).transport.volume.level;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    fixture.executor->drain();
    CHECK(currentPlayback(fixture).transport.volume.level < before);
    mouse.button = ftxui::Mouse::Left;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    fixture.executor->drain();
    CHECK(currentPlayback(fixture).transport.volume.muted);
  }

  TEST_CASE("EventController - foreground detail content scrolls and blocks controls under its painted cells",
            "[tui][regression][mouse][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    prepareSeekablePlayback(fixture, library);
    auto controller = fixture.makeEvents(library);
    fixture.shell.openOverlay(Overlay::DetailPanel);
    fixture.hitRegions.overlayPanel.box = {.x_min = 0, .x_max = 79, .y_min = 0, .y_max = 10};
    fixture.hitRegions.overlayPanel.contentBox = {.x_min = 0, .x_max = 79, .y_min = 0, .y_max = 40};
    fixture.hitRegions.overlayPanel.closeBox = {.x_min = 78, .x_max = 78, .y_min = 0, .y_max = 0};
    fixture.hitRegions.volumeBox = {.x_min = 70, .x_max = 75, .y_min = 0, .y_max = 0};
    auto mouse = ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 72, .y = 0};
    auto const volume = currentPlayback(fixture).transport.volume.level;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(fixture.shell.overlayScroll() == fixture.preferences.wheelStep);
    CHECK(library.selectedTrack() == 0);
    CHECK(currentPlayback(fixture).transport.volume.level == volume);
    mouse.button = ftxui::Mouse::Left;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    fixture.executor->drain();
    CHECK_FALSE(currentPlayback(fixture).transport.volume.muted);
    mouse.x = 78;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - disabled mouse input also protects an open Settings editor",
            "[tui][regression][mouse][settings]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.settingsPtr->open();
    auto const rendered = renderElement(fixture.settingsPtr->renderModal(80, 24), 80, 24);
    auto const optBox = findTextCells(rendered.screen, "Appearance");
    REQUIRE(optBox);
    auto const click = ftxui::Event::Mouse(
      "",
      ftxui::Mouse{
        .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optBox->x_min, .y = optBox->y_min});
    fixture.preferences.mouseEnabled = false;
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(fixture.settingsPtr->page() == SettingsPage::General);
    fixture.preferences.mouseEnabled = true;
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(fixture.settingsPtr->page() == SettingsPage::Appearance);
  }

  TEST_CASE("EventController - exact commands win until the user navigates candidates", "[tui][regression][input]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(
      library,
      defaultKeymapPlan(),
      [](std::string_view draft, std::size_t) -> std::optional<rt::CompletionResult>
      {
        return rt::CompletionResult{.replaceBegin = 0,
                                    .replaceEnd = draft.size(),
                                    .items = {rt::CompletionItem{.displayText = "output", .insertText = "output"},
                                              rt::CompletionItem{.displayText = "detail", .insertText = "detail"}}};
      });
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(":")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("detail")));

    SECTION("A literal alias is authoritative")
    {
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Return));
      CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    }

    SECTION("Deliberate candidate navigation activates the candidate")
    {
      REQUIRE(controller.tryHandleEvent(ftxui::Event::ArrowDown));
      REQUIRE(controller.tryHandleEvent(ftxui::Event::ArrowUp));
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Return));
      CHECK(fixture.shell.overlay() == Overlay::OutputDevices);
    }
  }

  TEST_CASE("EventController - local list search cannot activate a hidden row", "[tui][regression][search]")
  {
    auto fixture = EventControllerFixture{};
    auto const jazz = fixture.addList("Jazz");
    fixture.addList("Rock");
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("l")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("missing")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.isNavigationFocused());
    CHECK(library.currentListId() == rt::kAllTracksListId);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.isNavigationFocused());
    CHECK_FALSE(fixture.shell.listSearch().isActive());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("jazz")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(library.currentListId() == jazz);
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - read-only panels use viewport paging and boundary navigation",
            "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.openOverlay(Overlay::Help);
    fixture.hitRegions.overlayPanel.box = {.x_max = 30, .y_min = 1, .y_max = 9};
    fixture.hitRegions.overlayPanel.navigationBox = {.x_max = 30, .y_min = 2, .y_max = 6};
    fixture.hitRegions.overlayPanel.contentBox = {.x_max = 30, .y_max = 99};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::PageDown));
    CHECK(fixture.shell.overlayScroll() == 5);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("j")));
    CHECK(fixture.shell.overlayScroll() == 6);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::End));
    CHECK(fixture.shell.overlayScroll() == 99);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::End));
    CHECK(fixture.shell.overlayScroll() == 99);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Home));
    CHECK(fixture.shell.overlayScroll() == 0);
  }

  TEST_CASE("EventController - moving the filter caret refreshes completion without rescheduling work",
            "[tui][regression][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();
    std::size_t seenCursor = 0;
    auto controller =
      fixture.makeEvents(library,
                         defaultKeymapPlan(),
                         {},
                         [&](std::string_view, std::size_t cursor) -> std::optional<rt::CompletionResult>
                         {
                           seenCursor = cursor;
                           return std::nullopt;
                         });
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("First")));
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::ArrowLeft));
    CHECK(seenCursor == 4);
    CHECK(fixture.sleeperPtr->callCount() == 1);
    REQUIRE(fixture.sleeperPtr->tryFire(0));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return library.filterDraft() == "First"; }));
    CHECK(fixture.shell.inputDraft() == "First");
  }

  TEST_CASE("EventController - clicking outside a popover dismisses it without activating the workspace",
            "[tui][regression][mouse][popover]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.overlayPanel.box = {.x_min = 20, .x_max = 50, .y_min = 5, .y_max = 15};
    fixture.hitRegions.trackTableBox = {.x_min = 0, .x_max = 79, .y_min = 2, .y_max = 20};
    fixture.hitRegions.trackRows = {TrackRowHitRegion{
      .id = library.tracks()[1].id, .rowIndex = 1, .box = {.x_min = 0, .x_max = 19, .y_min = 4, .y_max = 4}}};
    fixture.hitRegions.soulButtonBox = {.x_min = 0, .x_max = 19, .y_min = 4, .y_max = 4};
    auto const click = ftxui::Event::Mouse(
      "", ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 4});

    for (auto const overlay : {Overlay::QualityPanel,
                               Overlay::OutputDevices,
                               Overlay::PresentationPanel,
                               Overlay::Notifications,
                               Overlay::Help})
    {
      REQUIRE(controller.tryHandleEvent(
        ftxui::Event::Mouse("", ftxui::Mouse{.motion = ftxui::Mouse::Moved, .x = 10, .y = 4})));
      REQUIRE(controller.isQualityHoverVisible());
      fixture.shell.openOverlay(overlay);
      REQUIRE(controller.tryHandleEvent(click));
      CHECK(fixture.shell.overlay() == Overlay::None);
      CHECK_FALSE(controller.isQualityHoverVisible());
      CHECK(library.selectedTrack() == 0);
      CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == kInvalidTrackId);
    }

    fixture.hitRegions.settingsButtonBox = {.x_min = 0, .x_max = 19, .y_min = 4, .y_max = 4};
    fixture.shell.openOverlay(Overlay::OutputDevices);
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK_FALSE(fixture.settingsPtr->isActive());
  }

  TEST_CASE("EventController - popovers retain inside clicks and ignore outside wheel or release events",
            "[tui][regression][mouse][popover]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.openOverlay(Overlay::Help);
    fixture.hitRegions.overlayPanel.box = {.x_min = 20, .x_max = 50, .y_min = 5, .y_max = 15};
    auto mouse = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 30, .y = 10};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(fixture.shell.overlay() == Overlay::Help);
    mouse.x = 0;
    mouse.button = ftxui::Mouse::WheelDown;
    controller.tryHandleEvent(ftxui::Event::Mouse("", mouse));
    CHECK(fixture.shell.overlay() == Overlay::Help);
    mouse.button = ftxui::Mouse::Left;
    mouse.motion = ftxui::Mouse::Released;
    controller.tryHandleEvent(ftxui::Event::Mouse("", mouse));
    CHECK(fixture.shell.overlay() == Overlay::Help);
  }

  TEST_CASE("EventController - outside clicks cancel command input and preserve the underlying inspector",
            "[tui][regression][mouse][popover]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.openOverlay(Overlay::DetailPanel);
    fixture.shell.beginInput(ShellInputMode::Command, "settings");
    fixture.hitRegions.inputPanel.box = {.x_min = 20, .x_max = 60, .y_min = 5, .y_max = 15};
    fixture.hitRegions.settingsButtonBox = {.x_min = 0, .x_max = 10, .y_min = 0, .y_max = 0};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse(
      "", ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 5, .y = 0})));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK_FALSE(fixture.settingsPtr->isActive());
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
  }

  TEST_CASE("EventController - Quick Filter outside clicks keep literal text and cancel pending completion",
            "[tui][regression][popover][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library, defaultKeymapPlan(), {}, completeYuduo);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("yuduo")));
    REQUIRE(fixture.shell.commandCompletion());
    REQUIRE(fixture.sleeperPtr->tryWaitForCallCount(1));
    fixture.hitRegions.inputPanel.box = {.x_min = 20, .x_max = 60, .y_min = 5, .y_max = 15};
    fixture.hitRegions.completion.inputBox = {.x_min = 0, .x_max = 79, .y_min = 23, .y_max = 23};
    auto mouse = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 0, .y = 23};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK(fixture.shell.isInputActive());
    mouse.y = 0;
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft() == "yuduo");
    CHECK(fixture.sleeperPtr->tryWaitForCancellation(0));
    CHECK_FALSE(fixture.sleeperPtr->tryFire(0));
  }

  TEST_CASE("EventController - outside clicks on untouched filter input preserve the applied filter",
            "[tui][regression][popover][filter]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    library.setFilterDraft("First");
    REQUIRE(library.applyFilter());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));
    fixture.hitRegions.inputPanel.box = {.x_min = 20, .x_max = 60, .y_min = 5, .y_max = 15};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse(
      "", ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 0, .y = 0})));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft() == "First");
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.selectedTrackView().track->row.title == "First");
  }

  TEST_CASE("EventController - outside clicks retain side panels and disabled mouse popovers",
            "[tui][regression][mouse][popover]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.overlayPanel.box = {.x_min = 20, .x_max = 50, .y_min = 5, .y_max = 15};
    auto const click = ftxui::Event::Mouse(
      "", ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 0, .y = 0});

    fixture.shell.openOverlay(Overlay::DetailPanel);
    controller.tryHandleEvent(click);
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    fixture.shell.openOverlay(Overlay::OutputDevices);
    fixture.preferences.mouseEnabled = false;
    REQUIRE(controller.tryHandleEvent(click));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);
  }

  TEST_CASE("EventController - selection footer cancels a range while retaining the detail pane",
            "[tui][regression][usability]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    library.toggleFocusedMark();
    auto const originalMarks = library.markedIds();
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("v")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::ArrowDown));
    fixture.shell.openOverlay(Overlay::DetailPanel);
    fixture.hitRegions.cancelSelectionBox = {.x_min = 20, .x_max = 40, .y_min = 23, .y_max = 23};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse(
      "", ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 25, .y = 23})));
    CHECK_FALSE(library.isVisualSelectionActive());
    CHECK(library.markedIds() == originalMarks);
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
  }

  TEST_CASE("EventController - browse overlays admit transport while retaining navigation and activation",
            "[tui][regression][keyboard][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    prepareSeekablePlayback(fixture, library);
    auto const track = currentPlayback(fixture).transport.nowPlaying.trackId;

    for (auto const overlay : {Overlay::QualityPanel,
                               Overlay::Help,
                               Overlay::OutputDevices,
                               Overlay::PresentationPanel,
                               Overlay::Notifications})
    {
      fixture.shell.openOverlay(overlay);
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(" ")));
      fixture.executor->drain();
      CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Paused);
      CHECK(fixture.shell.overlay() == overlay);
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(" ")));
      fixture.executor->drain();
      CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Playing);
      CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == track);
    }

    fixture.shell.openOverlay(Overlay::Help);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::F1));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - text entry and editors own playback letters and space",
            "[tui][regression][keyboard][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    prepareSeekablePlayback(fixture, library);
    fixture.shell.focusNavigation();
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));

    for (auto const* value : {" ", "S", "r", ">"})
    {
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(value)));
    }

    fixture.executor->drain();
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Playing);
    CHECK(currentPlayback(fixture).succession.shuffle == rt::ShuffleMode::Off);
    CHECK(currentPlayback(fixture).succession.repeat == rt::RepeatMode::Off);
    CHECK(library.navigation().search().matches(" Sr>"));
    CHECK_FALSE(library.navigation().search().matches("Other"));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("e")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("r")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(" ")));
    fixture.executor->drain();
    CHECK(currentPlayback(fixture).transport.transport == audio::Transport::Playing);
    CHECK(currentPlayback(fixture).succession.repeat == rt::RepeatMode::Off);
  }

  TEST_CASE("EventController - transport keys change the playback sequence without moving library focus",
            "[tui][unit][keyboard][event]")
  {
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    auto const first = library.tracks()[0].id;
    auto const second = library.tracks()[1].id;
    prepareSeekablePlayback(fixture, library);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character(">")));
    REQUIRE(fixture.tryWaitForPlayback(second));
    CHECK(library.selectedTrack() == 0);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::ArrowLeftCtrl));
    REQUIRE(fixture.tryWaitForPlayback(first));
    CHECK(library.selectedTrack() == 0);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("S")));
    fixture.executor->drain();
    CHECK(currentPlayback(fixture).succession.shuffle == rt::ShuffleMode::On);

    for (auto const repeat : {rt::RepeatMode::All, rt::RepeatMode::One, rt::RepeatMode::Off})
    {
      REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("r")));
      fixture.executor->drain();
      CHECK(currentPlayback(fixture).succession.repeat == repeat);
    }
  }

  TEST_CASE("EventController - Lists focus suspends track selection and keeps search-local bindings",
            "[tui][regression][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto const target = fixture.addList("Target");
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({{"tui.workspace.switchFocus", {"Z"}}});
    auto const plan = KeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, plan);
    fixture.hitRegions.navigationLayout = navigationGeometry(120, 0, true, true, false);
    library.toggleVisualSelection();
    library.moveTrackSelection(1);
    auto const marked = library.selectedTrackIds();
    auto const view = library.activeViewId();
    auto const selection = fixture.runtimePtr->views().trackListState(view).selection;
    fixture.shell.focusNavigation();
    REQUIRE(controller.tryHandleEvent(ftxui::Event::ArrowDown));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("v")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("C")));
    CHECK(library.selectedTrackIds() == marked);
    CHECK(library.isVisualSelectionActive());
    CHECK(library.activeViewId() == view);
    CHECK(fixture.runtimePtr->views().trackListState(view).selection == selection);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("z")));
    CHECK(library.navigation().search().query() == "z");
    CHECK(fixture.shell.isNavigationFocused());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Tab));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK_FALSE(library.navigation().search().isActive());
    CHECK(library.selectedTrackIds() == marked);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("z")));
    CHECK(fixture.shell.isNavigationFocused());
    library.navigation().reveal(target);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(library.currentListId() == target);
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(fixture.shell.isNavigationEnabled());
  }

  TEST_CASE("EventController - stronger surfaces suspend and restore Lists search", "[tui][unit][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.focusNavigation();
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("?")));
    CHECK(fixture.shell.overlay() == Overlay::Help);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.isNavigationFocused());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("All")));
    fixture.shell.beginInput(ShellInputMode::Command);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.isNavigationFocused());
    CHECK(library.navigation().search().query() == "All");
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.isNavigationFocused());
    CHECK_FALSE(library.navigation().search().isActive());
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(fixture.shell.isNavigationEnabled());
  }

  TEST_CASE("EventController - drawer outside press consumes Tracks while docked press operates Tracks",
            "[tui][regression][mouse][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.focusNavigation();
    fixture.hitRegions.navigationLayout = navigationGeometry(80, 0, true, true, false);
    fixture.hitRegions.navigation.panel.box = {.x_min = 0, .x_max = 25, .y_min = 2, .y_max = 20};
    fixture.hitRegions.trackTableBox = {.x_min = 0, .x_max = 79, .y_min = 2, .y_max = 20};
    fixture.hitRegions.trackRows = {
      {.id = library.tracks()[1].id, .rowIndex = 1, .box = {.x_min = 26, .x_max = 70, .y_min = 5, .y_max = 5}}};
    auto const click =
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 40, .y = 5});
    REQUIRE(controller.tryHandleEvent(click));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(fixture.shell.isNavigationEnabled());
    CHECK(library.selectedTrack() == 0);
    fixture.shell.focusNavigation();
    fixture.hitRegions.navigationLayout = navigationGeometry(120, 0, true, true, false);
    REQUIRE(controller.tryHandleEvent(click));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(library.selectedTrack() == 1);
    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("EventController - docked List wheel and disclosure never navigate or steal focus",
            "[tui][regression][mouse][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto const target = fixture.addList("Wheel target");
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.navigationLayout = navigationGeometry(120, 0, true, false, false);
    auto const rendered = renderElement(navigationPanel(ao::test::englishMessageCatalog(),
                                                        library.navigation(),
                                                        library.currentListId(),
                                                        defaultKeymapPlan(),
                                                        {.columns = 26, .regions = &fixture.hitRegions.navigation}),
                                        26,
                                        15);
    auto const view = library.activeViewId();
    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 5, .y = 4})));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(library.activeViewId() == view);
    CHECK(library.navigation().cursor() == target);
    fixture.preferences.mouseEnabled = false;
    auto const cursor = library.navigation().cursor();
    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::WheelUp, .motion = ftxui::Mouse::Pressed, .x = 5, .y = 4})));
    CHECK(library.navigation().cursor() == cursor);
  }

  TEST_CASE("EventController - track scrollbar and resize take focus before starting a gesture",
            "[tui][regression][mouse][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.navigationLayout = navigationGeometry(120, 0, true, true, false);
    fixture.hitRegions.trackTableBox = {.x_min = 26, .x_max = 119, .y_min = 2, .y_max = 20};
    fixture.shell.focusNavigation();
    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 119, .y = 20})));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK(library.selectedTrack() == 1);
    controller.cancelTransientInteractions();
    fixture.shell.focusNavigation();
    fixture.hitRegions.trackColumnResizeHandles = {
      {.field = rt::TrackField::Title, .box = {.x_min = 26, .x_max = 56, .y_min = 2, .y_max = 2}, .columns = 30}};
    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 56, .y = 2})));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 61, .y = 2})));
    CHECK(fixture.trackColumnResizePreview.listId == library.currentListId());
    fixture.hitRegions.navigationLayout = navigationGeometry(80, 0, true, false, false);
    controller.syncWorkspaceGeometry();
    CHECK(fixture.trackColumnResizePreview.listId == kInvalidListId);
  }

  TEST_CASE("EventController - only explicit List visibility changes request a layout save", "[tui][unit][navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.hitRegions.navigationLayout = navigationGeometry(80, 0, true, false, false);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("l")));
    CHECK(fixture.layoutCheckpointCount == 0);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.layoutCheckpointCount == 0);
    fixture.hitRegions.navigationLayout = navigationGeometry(120, 0, true, false, false);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Tab));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::ArrowDown));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::TabReverse));
    CHECK(fixture.layoutCheckpointCount == 0);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("l")));
    CHECK_FALSE(fixture.shell.isNavigationEnabled());
    CHECK(fixture.layoutCheckpointCount == 1);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("l")));
    CHECK(fixture.shell.isNavigationEnabled());
    CHECK(fixture.layoutCheckpointCount == 2);
  }

  TEST_CASE("EventController - search paging and scrollbar use visible rows with ancestor context",
            "[tui][regression][navigation]")
  {
    auto fixture = EventControllerFixture{};

    for (std::int32_t index = 0; index < 4; ++index)
    {
      auto const parentId = fixture.addList(std::format("Parent {}", index));
      REQUIRE(rt::test::runRuntimeTask(*fixture.runtimePtr,
                                       fixture.runtimePtr->library().commands().createListAsync(
                                         rt::ListDraft{.parentId = parentId, .name = "Needle"})));
    }

    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);
    fixture.shell.focusNavigation();
    fixture.hitRegions.navigationLayout = navigationGeometry(120, 0, true, true, false);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Character("Needle")));
    auto& model = library.navigation();
    REQUIRE(model.rows().size() == 8);
    model.selectVisibleRow(3);
    fixture.hitRegions.navigation.panel.navigationBox = {.x_min = 0, .x_max = 24, .y_min = 0, .y_max = 1};
    REQUIRE(controller.tryHandleEvent(ftxui::Event::PageDown));
    CHECK(model.selectedIndex() == 5);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::PageUp));
    CHECK(model.selectedIndex() == 3);
    fixture.hitRegions.navigation.revision = model.revision();
    fixture.hitRegions.navigation.panel.box = {.x_min = 0, .x_max = 25, .y_min = 0, .y_max = 8};
    fixture.hitRegions.navigation.panel.navigationBox = {.x_min = 0, .x_max = 24, .y_min = 0, .y_max = 7};
    REQUIRE(controller.tryHandleEvent(
      ftxui::Event::Mouse("", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 24, .y = 5})));
    CHECK(model.selectedIndex() == 5);
    CHECK(library.currentListId() == rt::kAllTracksListId);
  }
} // namespace ao::tui::test
