// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "tui/EventController.h"
#include "tui/HitRegions.h"
#include "tui/Keymap.h"
#include "tui/LibraryController.h"
#include "tui/LibraryScanController.h"
#include "tui/OutputDeviceController.h"
#include "tui/Preferences.h"
#include "tui/SettingsEditor.h"
#include "tui/ShellInteractionModel.h"
#include "tui/TrackEditController.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ao::library::test
{
  struct TrackSpec;
}

namespace ao::tui::test
{
  ftxui::Event clickBox(ftxui::Box const& box);

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

    explicit EventControllerFixture(bool useControlledSleeper = false);

    LibraryController makeLibrary();

    /// Every collaborator an EventController requires, all owned by this fixture.
    EventController makeEvents(LibraryController& library,
                               KeymapPlan const& keymapPlan = defaultKeymapPlan(),
                               InputCompletionCallback commandCompletion = {},
                               InputCompletionCallback filterCompletion = {});

    TrackId addTrack(library::test::TrackSpec const& spec) const;

    ListId addList(std::string name) const;

    void addReadyAudioProvider() const;

    void addReadyAudioProvider(audio::BackendProvider::Status status) const;

    bool tryWaitForPlayback(TrackId trackId);

    rt::PlaybackPositionRevision observedPositionRevision{};
  };
} // namespace ao::tui::test
