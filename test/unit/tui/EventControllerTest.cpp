// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/EventController.h"

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/tui/TuiTextCatalogTestSupport.h"
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
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_message.hpp>
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
      ftxui::ScreenInteractive screen{ftxui::ScreenInteractive::FixedSize(80, 24)};
      ShellInteractionModel shell{};

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

      LibraryController makeLibrary() const
      {
        return LibraryController{*runtimePtr, ao::test::englishPresentationTextCatalog(), englishTuiTextCatalog()};
      }

      TrackId addTrack(library::test::TrackSpec const& spec) const
      {
        return rt::test::addRuntimeTrack(*runtimePtr, spec, [this] { executor->drain(); });
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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

    REQUIRE(library.selectedTrack() == 0);

    CHECK(controller.handleEvent(ftxui::Event::Character("/")));
    CHECK(controller.handleEvent(ftxui::Event::ArrowDown));

    CHECK(fixture.shell.isInputActive());
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - Ctrl-C reaches global exit handling from every text input mode",
            "[tui][regression][event]")
  {
    auto requireExitHandlingFromInput = [](std::string const& opener)
    {
      auto fixture = EventControllerFixture{};
      auto library = fixture.makeLibrary();
      prepareSeekablePlayback(fixture, library);
      auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

      REQUIRE(currentPlayback(fixture).transport.nowPlaying.trackId != kInvalidTrackId);
      REQUIRE(controller.handleEvent(ftxui::Event::Character(opener)));
      REQUIRE(fixture.shell.isInputActive());

      CHECK(controller.handleEvent(ftxui::Event::CtrlC));
      CHECK(currentPlayback(fixture).transport.nowPlaying.trackId == kInvalidTrackId);
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

  TEST_CASE("EventController - cancelling untouched Quick Filter preserves the active filter",
            "[tui][regression][event][filter]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    library.setFilterDraft("First");
    REQUIRE(library.applyFilter());
    REQUIRE(library.tracks().size() == 1);
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      *fixture.runtimePtr,
                                      EventControllerBindings{.filterCompletionCallback = completeYuduo}};

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
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      *fixture.runtimePtr,
                                      EventControllerBindings{.filterCompletionCallback = completeYuduo}};

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
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      *fixture.runtimePtr,
                                      EventControllerBindings{.filterCompletionCallback = completeYuduo}};

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
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      *fixture.runtimePtr,
      EventControllerBindings{.notifications = &fixture.runtimePtr->notifications()},
    };

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

  TEST_CASE("EventController - destruction cancels a pending Quick Filter debounce", "[tui][unit][filter][concurrency]")
  {
    auto fixture = EventControllerFixture{true};
    auto library = fixture.makeLibrary();

    {
      auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};
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
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      *fixture.runtimePtr,
      EventControllerBindings{
        .notifications = &fixture.runtimePtr->notifications(),
        .commandCompletionCallback = [](std::string_view const draft) -> std::optional<rt::CompletionResult>
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
        }}};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

    CHECK(controller.handleEvent(ftxui::Event::Character(":")));
    CHECK(controller.handleEvent(ftxui::Event::Character("h")));
    CHECK(fixture.shell.isInputActive());
    CHECK(fixture.shell.inputDraft() == "h");

    CHECK(controller.handleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.shell.isInputActive());
    CHECK(fixture.shell.inputDraft().empty());
  }

  TEST_CASE("EventController - every declared key binding is answered", "[tui][unit][event]")
  {
    // The binding table is what the status bar and the command palette show the
    // user. A key listed there that the controller does not answer is a
    // shortcut the shell advertises and then ignores.
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

    for (auto const& binding : keyBindingSpecs())
    {
      INFO("key " << binding.key);

      // Quit ends the loop, and the overlay-closing key is only meaningful with
      // something open, so both are covered by their own cases instead.
      if (binding.action == CommandAction::Quit || binding.action == CommandAction::CloseOverlay)
      {
        continue;
      }

      auto const optEvent = [&binding] -> std::optional<ftxui::Event>
      {
        if (binding.key == "Enter")
        {
          return ftxui::Event::Return;
        }

        if (binding.key == "Space")
        {
          return ftxui::Event::Character(" ");
        }

        if (binding.key == "Ctrl-L")
        {
          return ftxui::Event::CtrlL;
        }

        return binding.key.size() == 1 ? std::optional{ftxui::Event::Character(std::string{binding.key})}
                                       : std::nullopt;
      }();

      REQUIRE(optEvent);
      CHECK(controller.handleEvent(*optEvent));

      // A key that opened an overlay has to close it again from inside that
      // overlay, which is a second dispatch site reading the same table. Help
      // is deliberately modal and leaves only on Escape.
      if (auto const opened = fixture.shell.overlay(); opened != Overlay::None && opened != Overlay::Help)
      {
        CHECK(controller.handleEvent(*optEvent));
        CHECK(fixture.shell.overlay() == Overlay::None);
      }

      // Leave no overlay open for the next binding to be judged against.
      fixture.shell.closeOverlay();
    }
  }

  TEST_CASE("EventController - detail shortcut toggles the detail overlay", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::DetailPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("d")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - detail follows the track table while it stays open", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 40, .y_min = 2, .y_max = 12};
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackColumnResizeHandles = {
      TrackColumnResizeHandle{.field = rt::TrackField::Title,
                              .box = ftxui::Box{.x_min = 8, .x_max = 20, .y_min = 2, .y_max = 2},
                              .columns = 20}};
    auto columnWidths = std::vector<TrackColumnWidthOverride>{};
    auto controller =
      EventController{fixture.screen,
                      fixture.shell,
                      library,
                      *fixture.runtimePtr,
                      EventControllerBindings{.hitRegions = &hitRegions, .trackColumnWidthOverrides = &columnWidths}};

    REQUIRE(controller.handleEvent(ftxui::Event::Character("d")));
    auto press = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 20, .y = 2};
    REQUIRE(controller.handleEvent(ftxui::Event::Mouse("", press)));

    // Help replaces Detail and blocks the workspace, so the drag stops here.
    CHECK(controller.handleEvent(ftxui::Event::Character("?")));
    REQUIRE(fixture.shell.overlay() == Overlay::Help);

    auto drag = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Moved, .x = 34, .y = 2};
    controller.handleEvent(ftxui::Event::Mouse("", drag));
    CHECK(columnWidths.empty());
  }

  TEST_CASE("EventController - another overlay replaces detail", "[tui][unit][event][detail]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 40, .y_min = 2, .y_max = 12};
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto outputDevices = OutputDeviceController{fixture.runtimePtr->playback(),
                                                ao::test::englishPresentationTextCatalog(),
                                                uimodel::OutputDeviceIntent::discarded()};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      *fixture.runtimePtr,
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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::PresentationPanel);

    CHECK(controller.handleEvent(ftxui::Event::Character("v")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - presentation shortcut selects track views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    CHECK(fixture.runtimePtr->views().trackListState(library.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("EventController - presentation navigation keys move within views", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      *fixture.runtimePtr,
      EventControllerBindings{.notifications = &fixture.runtimePtr->notifications()},
    };

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto outputDevices = OutputDeviceController{fixture.runtimePtr->playback(),
                                                ao::test::englishPresentationTextCatalog(),
                                                uimodel::OutputDeviceIntent::discarded()};
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
                      *fixture.runtimePtr,
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
    auto outputDevices = OutputDeviceController{fixture.runtimePtr->playback(),
                                                ao::test::englishPresentationTextCatalog(),
                                                uimodel::OutputDeviceIntent::discarded()};
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
                      *fixture.runtimePtr,
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
    auto outputDevices = OutputDeviceController{fixture.runtimePtr->playback(),
                                                ao::test::englishPresentationTextCatalog(),
                                                uimodel::OutputDeviceIntent::discarded()};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      *fixture.runtimePtr,
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
                  playbackBar(englishTuiTextCatalog(),
                              PlaybackBarViewState{.playbackState = &playbackSnapshot.transport,
                                                   .soulButtonBox = &hitRegions.soulButtonBox,
                                                   .terminalColumns = 80}));
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
                  playbackBar(englishTuiTextCatalog(),
                              PlaybackBarViewState{.playbackState = &playbackSnapshot.transport,
                                                   .soulButtonBox = &hitRegions.soulButtonBox,
                                                   .terminalColumns = 80}));
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      *fixture.runtimePtr,
      EventControllerBindings{.notifications = &fixture.runtimePtr->notifications()},
    };

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
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      *fixture.runtimePtr,
      EventControllerBindings{.notifications = &fixture.runtimePtr->notifications()},
    };

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
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      *fixture.runtimePtr,
      EventControllerBindings{.notifications = &fixture.runtimePtr->notifications()},
    };
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
    auto hitRegions = TuiHitRegions{};
    auto const albumsIndex = presentationIndex(library, "albums");
    REQUIRE(albumsIndex >= 0);
    hitRegions.presentationButtonBox = ftxui::Box{.x_min = 20, .x_max = 29, .y_min = 23, .y_max = 23};
    hitRegions.presentationRows = {PresentationRowHitRegion{
      .rowIndex = albumsIndex, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto activityStatusViewModel = uimodel::ActivityStatusViewModel{fixture.runtimePtr->notifications(),
                                                                    ao::test::englishPresentationTextCatalog(),
                                                                    [](uimodel::ActivityStatusViewState const&) {}};
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      *fixture.runtimePtr,
                                      EventControllerBindings{.activityStatusViewModel = &activityStatusViewModel}};

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);

    fixture.runtimePtr->notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::history());

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);

    CHECK(controller.handleEvent(ftxui::Event::Character("n")));
    CHECK(fixture.shell.overlay() == Overlay::None);
  }

  TEST_CASE("EventController - panel actions use transient activity notifications when available", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto activityStatusViewModel = uimodel::ActivityStatusViewModel{
      fixture.runtimePtr->notifications(),
      ao::test::englishPresentationTextCatalog(),
      [](uimodel::ActivityStatusViewState const&) {},
    };
    auto controller = EventController{fixture.screen,
                                      fixture.shell,
                                      library,
                                      *fixture.runtimePtr,
                                      EventControllerBindings{
                                        .activityStatusViewModel = &activityStatusViewModel,
                                        .notifications = &fixture.runtimePtr->notifications(),
                                      }};

    CHECK(controller.handleEvent(ftxui::Event::Character("l")));

    CHECK(fixture.shell.overlay() == Overlay::ListChooser);
    CHECK(activityStatusViewModel.viewState().compact.kind == uimodel::ActivityStatusKind::Info);
    CHECK(activityStatusViewModel.viewState().compact.text == "Lists");
    CHECK_FALSE(activityStatusViewModel.viewState().compact.optAutoDismissTimeout);
    auto const feed = fixture.runtimePtr->notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().lifetime == rt::NotificationLifetime::transient());
  }

  TEST_CASE("EventController - notification mouse targets hide only activity presentation", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto activityStatusViewModel = uimodel::ActivityStatusViewModel{fixture.runtimePtr->notifications(),
                                                                    ao::test::englishPresentationTextCatalog(),
                                                                    [](uimodel::ActivityStatusViewState const&) {}};
    fixture.runtimePtr->notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::history());
    auto const notificationId = fixture.runtimePtr->notifications().feed().entries.front().id;
    auto hitRegions = TuiHitRegions{};
    hitRegions.activityStatusBox = ftxui::Box{.x_min = 0, .x_max = 24, .y_min = 23, .y_max = 23};
    hitRegions.notificationDetailRows = {NotificationDetailRowHitRegion{
      .id = notificationId, .dismissible = true, .box = ftxui::Box{.x_min = 2, .x_max = 40, .y_min = 12, .y_max = 12}}};
    auto controller = EventController{
      fixture.screen,
      fixture.shell,
      library,
      *fixture.runtimePtr,
      EventControllerBindings{.hitRegions = &hitRegions, .activityStatusViewModel = &activityStatusViewModel}};

    auto clickActivity = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 8, .y = 23};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickActivity)));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);

    auto clickRow = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 12};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickRow)));
    CHECK(activityStatusViewModel.viewState().detail.items.empty());
    CHECK(fixture.runtimePtr->notifications().feed().entries.size() == 1);
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
                      *fixture.runtimePtr,
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
                      *fixture.runtimePtr,
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
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};
    enterQuickFilter(controller, "missing");
    REQUIRE(library.tracks().empty());

    auto hitRegions = TuiHitRegions{};
    hitRegions.trackTableBox = ftxui::Box{.x_min = 0, .x_max = 79, .y_min = 1, .y_max = 22};
    auto controllerWithTable = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

    auto clickSection = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 10, .y = 6};
    CHECK(controller.handleEvent(ftxui::Event::Mouse("", clickSection)));
    CHECK(library.selectedTrack() == 0);
  }

  TEST_CASE("EventController - list chooser return opens the selected list", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};
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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};
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

    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](std::chrono::milliseconds const elapsed) noexcept
                                                      { seekPreviews.push_back(elapsed); });
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot) noexcept
                                                    { snapshots.push_back(snapshot); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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

    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto snapshots = std::vector<rt::PlaybackSnapshot>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](std::chrono::milliseconds const elapsed) noexcept
                                                      { seekPreviews.push_back(elapsed); });
    auto snapshotSub = playback.events().onSnapshot([&snapshots](rt::PlaybackSnapshot const& snapshot) noexcept
                                                    { snapshots.push_back(snapshot); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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

  TEST_CASE("EventController - disabled seek rail ignores mouse clicks", "[tui][unit][event]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = fixture.runtimePtr->playback().events().onSeekPreview(
      [&seekPreviews](std::chrono::milliseconds const elapsed) noexcept { seekPreviews.push_back(elapsed); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = fixture.runtimePtr->playback().events().onSeekPreview(
      [&seekPreviews](std::chrono::milliseconds const elapsed) noexcept { seekPreviews.push_back(elapsed); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto hitRegions = TuiHitRegions{};
    hitRegions.seekRailBox = ftxui::Box{.x_min = 10, .x_max = 30, .y_min = 1, .y_max = 1};
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = fixture.runtimePtr->playback().events().onSeekPreview(
      [&seekPreviews](std::chrono::milliseconds const elapsed) noexcept { seekPreviews.push_back(elapsed); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto outputDevices = OutputDeviceController{fixture.runtimePtr->playback(),
                                                ao::test::englishPresentationTextCatalog(),
                                                uimodel::OutputDeviceIntent::discarded()};
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
      *fixture.runtimePtr,
      EventControllerBindings{
        .outputDevices = &outputDevices, .hitRegions = &hitRegions, .trackColumnWidthOverrides = &widthOverrides}};

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
    auto& playback = fixture.runtimePtr->playback();
    auto seekPreviews = std::vector<std::chrono::milliseconds>{};
    auto previewSub = playback.events().onSeekPreview([&seekPreviews](std::chrono::milliseconds const elapsed) noexcept
                                                      { seekPreviews.push_back(elapsed); });
    auto controller = EventController{
      fixture.screen, fixture.shell, library, *fixture.runtimePtr, EventControllerBindings{.hitRegions = &hitRegions}};

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
    auto controller = EventController{fixture.screen, fixture.shell, library, *fixture.runtimePtr};

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
