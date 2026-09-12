// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EventControllerTestSupport.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "tui/EventController.h"
#include "tui/Keymap.h"
#include "tui/LibraryController.h"
#include "tui/LibraryScanController.h"
#include "tui/Preferences.h"
#include "tui/SettingsEditor.h"
#include "tui/TerminalTitleFormat.h"
#include "tui/TrackEditController.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/BackendProvider.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/box.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
  } // namespace

  ftxui::Event clickBox(ftxui::Box const& box)
  {
    REQUIRE_FALSE(box.IsEmpty());
    return ftxui::Event::Mouse(
      "", {.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = box.x_min, .y = box.y_min});
  }

  EventControllerFixture::EventControllerFixture(bool const useControlledSleeper)
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

  LibraryController EventControllerFixture::makeLibrary()
  {
    return LibraryController{runtimePtr->library(),
                             runtimePtr->views(),
                             runtimePtr->workspace(),
                             ao::test::englishMessageCatalog(),
                             listPresentations};
  }

  EventController EventControllerFixture::makeEvents(LibraryController& library,
                                                     KeymapPlan const& keymapPlan,
                                                     InputCompletionCallback commandCompletion,
                                                     InputCompletionCallback filterCompletion)
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
      SettingsEditor::Outputs{
        .applyPreferences = [&](Preferences const& candidate) -> Result<>
        {
          preferences = candidate;
          return {};
        },
        .applyKeymap = [&](uimodel::KeymapModel const& candidate) -> Result<>
        {
          settingsKeymap = candidate;
          return {};
        },
        .coverMode = [] { return std::string{"off"}; },
        .previewTerminalTitle = [](std::string_view expression) -> Result<std::optional<std::string>>
        {
          auto planRes = compileTerminalTitleFormat(expression);

          if (!planRes)
          {
            return std::unexpected{planRes.error()};
          }

          return *planRes ? std::optional{std::string{"Aobus"}} : std::nullopt;
        }});
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

  TrackId EventControllerFixture::addTrack(library::test::TrackSpec const& spec) const
  {
    return rt::test::addRuntimeTrack(*runtimePtr, spec, [this] { executor->drain(); });
  }

  ListId EventControllerFixture::addList(std::string name) const
  {
    return ao::test::requireValue(rt::test::runRuntimeTask(
      *runtimePtr, runtimePtr->library().commands().createListAsync(rt::ListDraft{.name = std::move(name)})));
  }

  void EventControllerFixture::addReadyAudioProvider() const
  {
    rt::test::addReadyAudioProvider(*runtimePtr);
    executor->drain();
  }

  void EventControllerFixture::addReadyAudioProvider(audio::BackendProvider::Status status) const
  {
    rt::test::addReadyAudioProvider(*runtimePtr, std::move(status));
    executor->drain();
  }

  bool EventControllerFixture::tryWaitForPlayback(TrackId const trackId)
  {
    auto const settled = rt::test::tryWaitForPlaybackSettlement(
      *executor,
      observedPositionRevision,
      [this] { return runtimePtr->playback().snapshot().transport.positionRevision; });
    observedPositionRevision = runtimePtr->playback().snapshot().transport.positionRevision;
    return settled && runtimePtr->playback().snapshot().transport.nowPlaying.trackId == trackId;
  }
} // namespace ao::tui::test
