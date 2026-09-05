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
#include "test/unit/tui/TuiKeymapTestSupport.h"
#include "tui/LibraryController.h"
#include "tui/LibraryNavigation.h"
#include "tui/LibraryScanController.h"
#include "tui/NotificationCenterPanel.h"
#include "tui/OutputDeviceController.h"
#include "tui/OutputDevicePanel.h"
#include "tui/PlaybackPanel.h"
#include "tui/PresentationPanel.h"
#include "tui/ShellInteractionModel.h"
#include "tui/TerminalTrackColumnLayout.h"
#include "tui/TrackListEntry.h"
#include "tui/TrackPresentationNavigation.h"
#include "tui/TrackSection.h"
#include "tui/TrackTable.h"
#include "tui/TuiHitRegions.h"
#include "tui/TuiKeymap.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
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
      TuiHitRegions hitRegions{};
      uimodel::TrackColumnLayouts trackColumnLayouts{runtimePtr->library().changes()};
      TrackColumnResizePreview trackColumnResizePreview{};
      OutputDeviceController outputDevices{runtimePtr->playback(),
                                           ao::test::englishMessageCatalog(),
                                           uimodel::OutputDeviceIntent::discarded()};
      uimodel::ActivityStatusViewModel activityStatusViewModel{runtimePtr->notifications(),
                                                               ao::test::englishMessageCatalog(),
                                                               [](uimodel::ActivityStatusViewState const&) {}};
      std::unique_ptr<LibraryScanController> libraryScanPtr{};
      std::size_t exitRequestCount = 0;

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
                                 TuiKeymapPlan const& keymapPlan = defaultTuiKeymapPlan(),
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
                                 .requestExit = [this] { ++exitRequestCount; },
                                 .commandCompletionCallback = std::move(commandCompletion),
                                 .filterCompletionCallback = std::move(filterCompletion),
                               }};
      }

      TrackId addTrack(library::test::TrackSpec const& spec) const
      {
        return rt::test::addRuntimeTrack(*runtimePtr, spec, [this] { executor->drain(); });
      }

      ListId addList(std::string name) const
      {
        return ao::test::requireValue(rt::test::runRuntimeTask(
          *runtimePtr, runtimePtr->library().commands().createList(rt::ListDraft{.name = std::move(name)})));
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

      bool waitForPlayback(TrackId const trackId)
      {
        auto const settled = rt::test::waitForPlaybackSettlement(
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
      auto const it = std::ranges::find(library.libraryEntries(), listId, &LibraryNavEntry::id);
      REQUIRE(it != library.libraryEntries().end());
      auto const targetIndex = static_cast<std::int32_t>(it - library.libraryEntries().begin());
      library.moveFocusedSelection(true, targetIndex - library.selectedList());
      REQUIRE(library.openSelectedList().opened);
      REQUIRE(library.currentListId() == listId);
    }

    void prepareSeekablePlayback(EventControllerFixture& fixture, LibraryController const& library)
    {
      REQUIRE_FALSE(library.tracks().empty());
      fixture.addReadyAudioProvider();
      auto const startTrackId = library.tracks()[0].id;
      auto& playback = fixture.runtimePtr->playback();
      REQUIRE(playback.commands().startFromView(library.activeViewId(), startTrackId));
      REQUIRE(fixture.waitForPlayback(startTrackId));
      REQUIRE(playback.snapshot().transport.duration > std::chrono::milliseconds{0});
    }

    void enterCommand(EventController& controller, std::string_view text)
    {
      CHECK(controller.handleEvent(ftxui::Event::Character(":")));

      for (char const ch : text)
      {
        CHECK(controller.handleEvent(ftxui::Event::Character(std::string{ch})));
      }

      CHECK(controller.handleEvent(ftxui::Event::Return));
    }

    void enterQuickFilter(EventController& controller, std::string_view text)
    {
      CHECK(controller.handleEvent(ftxui::Event::Character("/")));

      for (char const ch : text)
      {
        CHECK(controller.handleEvent(ftxui::Event::Character(std::string{ch})));
      }

      CHECK(controller.handleEvent(ftxui::Event::Return));
    }

    std::optional<rt::CompletionResult> completeYuduo(std::string_view const draft)
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

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));

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
      REQUIRE(controller.handleEvent(ftxui::Event::Character(opener)));
      REQUIRE(fixture.shell.isInputActive());

      CHECK(controller.handleEvent(ftxui::Event::CtrlC));
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

    CHECK(controller.handleEvent(ftxui::Event::Character("q")));

    CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == trackId);
    CHECK(fixture.exitRequestCount == 1);
  }

  TEST_CASE("EventController - Ctrl-C and quit request the shared exit callback", "[tui][unit][event][exit]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character("q")));
    CHECK(fixture.exitRequestCount == 1);

    CHECK(controller.handleEvent(ftxui::Event::CtrlC));
    CHECK(fixture.exitRequestCount == 2);
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Custom));
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

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(fixture.shell.inputMode() == ShellInputMode::QuickFilter);
    CHECK(fixture.shell.inputDraft().empty());
    CHECK_FALSE(fixture.shell.isInputTouched());
    CHECK(library.filterDraft() == "First");

    CHECK(controller.handleEvent(ftxui::Event::Escape));
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

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK_FALSE(fixture.shell.isInputTouched());
    CHECK(controller.handleEvent(ftxui::Event::Return));

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

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Character("Second")));
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));
    CHECK(fixture.sleeperPtr->call(0).delay == std::chrono::milliseconds{200});
    CHECK(library.filterDraft().empty());

    REQUIRE(fixture.sleeperPtr->fire(0));
    REQUIRE(fixture.executor->drainUntil([&library] { return library.filterDraft() == "Second"; }));
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

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Character("F")));
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));
    REQUIRE(fixture.sleeperPtr->fire(0));
    REQUIRE(fixture.executor->drainUntil([&library] { return library.filterDraft() == "F"; }));
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.selectedTrackView().track->row.title == "First");

    CHECK(controller.handleEvent(ftxui::Event::Backspace));
    CHECK(fixture.shell.inputDraft().empty());
    REQUIRE(fixture.sleeperPtr->waitForCallCount(2));
    REQUIRE(fixture.sleeperPtr->fire(1));
    REQUIRE(fixture.executor->drainUntil([&library] { return library.filterDraft().empty(); }));

    CHECK(library.tracks().size() == 2);
    CHECK(fixture.shell.isInputActive());
  }

  TEST_CASE("EventController - Quick Filter Enter accepts completion and cancels pending debounce",
            "[tui][regression][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    fixture.addTrack(library::test::TrackSpec{.title = "First Love", .artist = "宇多田光"});
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library, defaultTuiKeymapPlan(), {}, completeYuduo);

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Character("yuduo")));
    REQUIRE(fixture.shell.commandCompletion());
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft() == "\"宇多田光\"");
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.selectedTrackView().track->row.artist == "宇多田光");
    CHECK(fixture.sleeperPtr->waitForCancellation(0));
    CHECK_FALSE(fixture.sleeperPtr->fire(0));
  }

  TEST_CASE("EventController - Quick Filter Escape applies literal text instead of the selected completion",
            "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library, defaultTuiKeymapPlan(), {}, completeYuduo);

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Character("yuduo")));
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(library.filterDraft() == "yuduo");
    CHECK(fixture.sleeperPtr->waitForCancellation(0));
  }

  TEST_CASE("EventController - Quick Filter Tab accepts completion and stays live", "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    fixture.addTrack(library::test::TrackSpec{.title = "First Love", .artist = "宇多田光"});
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library, defaultTuiKeymapPlan(), {}, completeYuduo);

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Character("yuduo")));
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));

    CHECK(controller.handleEvent(ftxui::Event::Tab));
    CHECK(fixture.shell.isInputActive());
    CHECK(fixture.shell.inputDraft() == "\"宇多田光\"");
    REQUIRE(fixture.sleeperPtr->waitForCancellation(0));
    REQUIRE(fixture.sleeperPtr->waitForCallCount(2));

    REQUIRE(fixture.sleeperPtr->fire(1));
    REQUIRE(fixture.executor->drainUntil([&library] { return library.filterDraft() == "\"宇多田光\""; }));
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

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Character("$artist =")));
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));
    REQUIRE(fixture.sleeperPtr->fire(0));
    REQUIRE(fixture.executor->drainUntil([&library] { return library.filterDraft() == "$artist ="; }));

    CHECK(fixture.shell.isInputActive());
    CHECK(library.filterError().contains("Filter error:"));
    CHECK(fixture.runtimePtr->notifications().feed().entries.empty());

    CHECK(controller.handleEvent(ftxui::Event::Return));
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
    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::Character("First")));
    REQUIRE(fixture.sleeperPtr->waitForCallCount(1));

    controller.cancelTransientInteractions();

    CHECK(fixture.sleeperPtr->waitForCancellation(0));
    CHECK(library.filterDraft().empty());
  }

  TEST_CASE("EventController - destruction cancels a pending Quick Filter debounce", "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();

    {
      auto controller = fixture.makeEvents(library);
      CHECK(controller.handleEvent(ftxui::Event::Character("/")));
      CHECK(controller.handleEvent(ftxui::Event::Character("First")));
      REQUIRE(fixture.sleeperPtr->waitForCallCount(1));
    }

    CHECK(fixture.sleeperPtr->waitForCancellation(0));
    CHECK(library.filterDraft().empty());
  }

  TEST_CASE("EventController - completion keys distinguish acceptance from submission", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller =
      fixture.makeEvents(library,
                         defaultTuiKeymapPlan(),
                         [](std::string_view const draft) -> std::optional<rt::CompletionResult>
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

    CHECK(controller.handleEvent(ftxui::Event::Character(":")));
    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(controller.handleEvent(ftxui::Event::Character("e")));
    REQUIRE(fixture.shell.commandCompletion());

    SECTION("Tab accepts the selection and keeps the Command Palette active")
    {
      CHECK(controller.handleEvent(ftxui::Event::Tab));
      CHECK(fixture.shell.isInputActive());
      CHECK(fixture.shell.inputDraft() == "detail");
      CHECK_FALSE(fixture.shell.commandCompletion());

      CHECK(controller.handleEvent(ftxui::Event::Return));
      CHECK_FALSE(fixture.shell.isInputActive());
      CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    }

    SECTION("Return rejects an unknown command without accepting the selection")
    {
      CHECK(controller.handleEvent(ftxui::Event::Return));
      CHECK(fixture.shell.isInputActive());
      CHECK(fixture.shell.inputDraft() == "de");
      CHECK(library.filterDraft().empty());
      CHECK(fixture.shell.overlay() == Overlay::None);
      auto const feed = fixture.runtimePtr->notifications().feed();
      REQUIRE(feed.entries.size() == 1);
      CHECK(feed.entries.front().severity == rt::NotificationSeverity::Warning);
      CHECK(std::get<std::string>(feed.entries.front().message) == "Unknown command: de");
    }

    SECTION("Page keys move by the bounded list page")
    {
      CHECK(controller.handleEvent(ftxui::Event::PageDown));
      CHECK(fixture.shell.commandCompletionSelection() == 1);
      CHECK(controller.handleEvent(ftxui::Event::PageDown));
      CHECK(fixture.shell.commandCompletionSelection() == 1);
      CHECK(controller.handleEvent(ftxui::Event::PageUp));
      CHECK(fixture.shell.commandCompletionSelection() == 0);
      CHECK(fixture.shell.isInputActive());
    }

    SECTION("Arrow keys cycle through completion items")
    {
      CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
      CHECK(fixture.shell.commandCompletionSelection() == 1);
      CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
      CHECK(fixture.shell.commandCompletionSelection() == 0);
      CHECK(controller.handleEvent(ftxui::Event::ArrowUp));
      CHECK(fixture.shell.commandCompletionSelection() == 1);
      CHECK(fixture.shell.isInputActive());
    }
  }

  TEST_CASE("EventController - command input escape cancels the draft", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character(":")));
    CHECK(controller.handleEvent(ftxui::Event::Character("h")));
    CHECK(fixture.shell.isInputActive());
    CHECK(fixture.shell.inputDraft() == "h");

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(fixture.shell.inputDraft().empty());
  }

  TEST_CASE("EventController - detail shortcut toggles the detail overlay", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - effective plan replaces old root keys instead of adding bypasses", "[tui][unit][keymap]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{tuiDefaultKeymap()};
    keymap.applyOverrides({
      {"tui.shell.toggleListChooser", {}},
      {"tui.shell.toggleTrackDetail", {"F2"}},
    });
    auto const keymapPlan = TuiKeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, keymapPlan);

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Character("l")));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.handleEvent(ftxui::Event::F2));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(controller.handleEvent(ftxui::Event::F2));
    CHECK(fixture.shell.overlay() == Overlay::None);

    enterCommand(controller, "lists");
    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
  }

  TEST_CASE("EventController - scoped protocol wins over conflicting root bindings", "[tui][unit][keymap]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{tuiDefaultKeymap()};
    keymap.applyOverrides({
      {"tui.shell.toggleListChooser", {"Enter"}},
      {"tui.shell.toggleTrackDetail", {"Down", "Q"}},
      {"tui.shell.quit", {"Escape"}},
    });
    auto const keymapPlan = TuiKeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, keymapPlan);

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    CHECK(library.selectedTrack() == 1);
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    REQUIRE(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    REQUIRE(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

    fixture.shell.beginInput(ShellInputMode::QuickFilter);
    CHECK(controller.handleEvent(ftxui::Event::Character("q")));
    CHECK(fixture.shell.inputDraft() == "q");
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - detail follows the track table while it stays open", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);
    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    CHECK(library.selectedTrack() == 1);
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Home));
    CHECK(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::End));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(library.tracks().size()) - 1);

    CHECK(controller.handleEvent(ftxui::Event::PageUp));
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

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("}")));
    CHECK(library.selectedTrack() == static_cast<std::int32_t>(secondSection.rowBegin));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("{")));
    CHECK(library.selectedTrack() == 0);

    // Text input is its own mode: it suspends the workspace without closing the
    // inspector, so Escape leaves Detail exactly as it was.
    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(fixture.shell.isInputActive());
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Escape));
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

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);

    // Enter starts the selection the inspector is describing.
    CHECK(controller.handleEvent(ftxui::Event::Return));
    REQUIRE(fixture.waitForPlayback(trackId));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);

    CHECK(controller.handleEvent(ftxui::Event::Character(" ")));
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

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 40, .y = 4};
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    auto const draggedSelection = library.selectedTrack();

    // Escape takes the pane away, so the drag has nothing left to aim at.
    CHECK(controller.handleEvent(ftxui::Event::Escape));
    REQUIRE(fixture.shell.overlay() == Overlay::None);

    auto drag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 40, .y = 11};
    controller.handleEvent(ftxui::Event::Mouse("", drag));
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

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    auto drag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 34, .y = 2};
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", drag)));
    REQUIRE_FALSE(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());

    // Help replaces Detail and blocks the workspace, so the preview rolls back.
    CHECK(controller.handleEvent(ftxui::Event::Character("?")));
    REQUIRE(fixture.shell.overlay() == Overlay::Help);
    CHECK(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());

    controller.handleEvent(ftxui::Event::Mouse("", drag));
    CHECK(columnLayouts.snapshot().empty());
  }

  TEST_CASE("EventController - another overlay replaces detail", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("?")));
    CHECK(fixture.shell.overlay() == Overlay::Help);

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);
    CHECK(controller.handleEvent(ftxui::Event::Escape));
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

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    REQUIRE(fixture.shell.overlay() == Overlay::DetailPanel);
    REQUIRE(library.selectedTrack() == 0);

    auto wheel = ftxui::Mouse{.button = ftxui::Mouse::WheelDown, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 5};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", wheel)));
    CHECK(library.selectedTrack() > 0);
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);
  }

  TEST_CASE("EventController - overlay shortcuts update visible shell state", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

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
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character("o")));
    CHECK(fixture.shell.overlay() == Overlay::OutputDevices);

    CHECK(controller.handleEvent(ftxui::Event::Character("o")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - modal overlays swallow workspace shortcuts", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{tuiDefaultKeymap()};
    keymap.applyOverrides({{"tui.shell.toggleListChooser", {"F2"}}});
    auto const keymapPlan = TuiKeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, keymapPlan);

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
      ftxui::Event::F2,
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

  TEST_CASE("EventController - notification x remains protocol-owned when there is nothing to dismiss",
            "[tui][unit][keymap]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{tuiDefaultKeymap()};
    keymap.applyOverrides({{"tui.shell.toggleNotifications", {"X"}}});
    auto const keymapPlan = TuiKeymapPlan{keymap};
    auto controller = fixture.makeEvents(library, keymapPlan);
    fixture.shell.openOverlay(Overlay::Notifications);

    CHECK(controller.handleEvent(ftxui::Event::Character("x")));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);
  }

  TEST_CASE("EventController - non-list modal overlays do not page the track table", "[tui][regression][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

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
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - presentation shortcut selects track views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    auto const albumsIndex = presentationIndex(library, "albums");
    REQUIRE(albumsIndex >= 0);
    CHECK(controller.handleEvent(ftxui::Event::Home));

    for (std::int32_t index = 0; index < albumsIndex; ++index)
    {
      CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    }

    CHECK(library.selectedPresentation() == albumsIndex);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtimePtr->views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - presentation navigation keys move within views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

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
    auto controller = fixture.makeEvents(library);

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
    CHECK(fixture.runtimePtr->views().trackListState(library.activeViewId()).presentation.id == "albums");

    enterCommand(controller, "reload");
    CHECK(library.tracks().size() == 2);

    enterCommand(controller, "play");
    REQUIRE(fixture.waitForPlayback(library.tracks()[library.selectedTrack()].id));
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
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));

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
    auto& hitRegions = fixture.hitRegions;
    hitRegions.outputDeviceButtonBox = ftxui::Box{.x_min = 4, .x_max = 9, .y_min = 0, .y_max = 0};
    hitRegions.libraryButtonBox = ftxui::Box{.x_min = 2, .x_max = 12, .y_min = 23, .y_max = 23};
    hitRegions.presentationButtonBox = ftxui::Box{.x_min = 15, .x_max = 24, .y_min = 23, .y_max = 23};
    hitRegions.activityStatusBox = ftxui::Box{.x_min = 28, .x_max = 48, .y_min = 23, .y_max = 23};
    auto controller = fixture.makeEvents(library);

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
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSoulButton)));
    REQUIRE(fixture.waitForPlayback(library.tracks()[library.selectedTrack()].id));
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
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character(" ")));

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

    CHECK(controller.handleEvent(ftxui::Event::Character("s")));

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
    REQUIRE(fixture.waitForPlayback(trackId));
    auto controller = fixture.makeEvents(library);
    auto const selected = playback.snapshot().transport.output.selectedDevice;

    commands.setOutputDevice(selected.backendId, audio::DeviceId{"pending-device"}, selected.profileId);

    auto transport = playback.snapshot().transport;
    REQUIRE_FALSE(transport.ready);
    REQUIRE(transport.transport == audio::Transport::Playing);
    auto const notificationCount = fixture.runtimePtr->notifications().feed().entries.size();

    CHECK(controller.handleEvent(ftxui::Event::Character(" ")));

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
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickView)));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 12};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.runtimePtr->views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - notification shortcut opens available activity details", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& activityStatusViewModel = fixture.activityStatusViewModel;
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);

    fixture.runtimePtr->notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::pinned());

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);
    REQUIRE(activityStatusViewModel.viewState().compact.dismissible);

    CHECK(controller.handleEvent(ftxui::Event::Character("x")));
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Idle);
    CHECK(fixture.shell.overlay() == Overlay::Notifications);

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - panel actions use transient activity notifications when available", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto& activityStatusViewModel = fixture.activityStatusViewModel;
    auto controller = fixture.makeEvents(library);

    CHECK(controller.handleEvent(ftxui::Event::Character("l")));

    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Info);
    CHECK(activityStatusViewModel.viewState().compact.text == "Lists");
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

    CHECK(controller.handleEvent(ftxui::Event::Escape));

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
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickActivity)));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 12};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));
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
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressEdge)));

    auto moveRight = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 25, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", moveRight)));
    CHECK(resizePreview.listId == rt::kAllTracksListId);
    REQUIRE_FALSE(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());
    CHECK(changedLists.empty());

    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 25, .y = 2};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", release)));
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
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", moveAfterRelease)));
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
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", press)));

    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 26, .y_min = 2, .y_max = 2},
                              .columns = 26,
                              .availableColumns = 130}};
    auto const move = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 25, .y = 2};
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", move)));
    auto const preview = projectTerminalTrackColumnLayout(
      library.activePresentation(), resizePreview.layout, hitRegions.trackColumnResizeHandles.front().availableColumns);
    auto const previewTitle = std::ranges::find(preview.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    REQUIRE(previewTitle != preview.columns.end());
    CHECK(previewTitle->columns == 25);

    auto const release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 25, .y = 2};
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", release)));
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
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", pressEdge)));
    auto firstMove = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 30, .y = 2};
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", firstMove)));
    REQUIRE_FALSE(resizePreview.layout.empty());

    auto secondPress = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 2, .y = 8};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", secondPress)));
    CHECK(resizePreview.layout.empty());

    auto staleMove = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 40, .y = 2};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", staleMove)));
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
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", move)));
    REQUIRE_FALSE(resizePreview.layout.empty());

    openList(library, otherListId);
    auto release = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = 28, .y = 2};
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", release)));

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
      auto const press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
      auto const release =
        ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Released, .x = releaseX, .y = 2};
      REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", press)));
      REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", release)));
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
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", move)));
    REQUIRE_FALSE(resizePreview.layout.empty());

    controller.cancelTransientInteractions();

    CHECK(resizePreview.layout.empty());
    CHECK(columnLayouts.snapshot().empty());
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
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controller = fixture.makeEvents(library);

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
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 2};
    auto controller = fixture.makeEvents(library);

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
    auto controller = fixture.makeEvents(library);
    enterQuickFilter(controller, "missing");
    REQUIRE(library.tracks().empty());

    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controllerWithTable = fixture.makeEvents(library);

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
    auto controller = fixture.makeEvents(library);

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
    auto controller = fixture.makeEvents(library);

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
    auto& hitRegions = fixture.hitRegions;
    hitRegions.trackSectionRows = {
      TrackSectionRowHitRegion{.sectionIndex = 1, .box = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 6, .y_max = 6}}};
    auto controller = fixture.makeEvents(library);

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));
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
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - list chooser return opens the selected list", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = fixture.makeEvents(library);

    fixture.shell.openOverlay(Overlay::ListChooser);

    CHECK(controller.handleEvent(ftxui::Event::Return));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(library.currentListTitle() == "All Tracks");
    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == rt::NotificationSeverity::Info);
    CHECK(std::get<std::string>(feed.entries.front().message) == "Opened All Tracks");
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
    auto controller = fixture.makeEvents(library);
    auto& playback = fixture.runtimePtr->playback();
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot) noexcept
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

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", release)));

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

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", dragOutside)));
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", releaseOutside)));

    REQUIRE(seekPreviews.size() == 2);
    CHECK(seekPreviews[0] == std::chrono::milliseconds{0});
    CHECK(seekPreviews[1] == duration);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].transport.elapsed == duration);
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

    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(seekPreviews.size() == 1);
    CHECK(seekPreviews[0] == before.duration);
    CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);

    controller.cancelTransientInteractions();

    auto const stabilized = playback.snapshot().transport;
    CHECK(stabilized.elapsed == before.elapsed);
    CHECK(stabilized.finalSeekRevision.value == before.finalSeekRevision.value + 1);
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", release)));
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

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
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

    fixture.shell.openOverlay(Overlay::ListChooser);
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
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

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 1};

    CHECK_FALSE(controller.handleEvent(ftxui::Event::Mouse("", press)));
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
    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    REQUIRE(fixture.shell.isInputActive());

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

    CHECK(controller.handleEvent(ftxui::Event::Mouse("", press)));
    REQUIRE(seekPreviews.size() == 1);
    CHECK(seekPreviews[0] == std::chrono::milliseconds{0});

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
    auto controller = fixture.makeEvents(library);

    REQUIRE(library.selectedTrack() == 0);
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));
    REQUIRE(library.selectedTrack() == 1);
    CHECK(controller.handleEvent(ftxui::Event::Character("p")));
    REQUIRE(fixture.waitForPlayback(library.tracks()[1].id));
    REQUIRE(currentPlayback(fixture).transport.nowPlaying.trackId == library.tracks()[1].id);

    CHECK(controller.handleEvent(ftxui::Event::Home));
    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::CtrlL));
    CHECK(library.selectedTrack() == 1);
  }
} // namespace ao::tui::test
