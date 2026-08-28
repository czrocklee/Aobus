// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::audio::test
{
  class StagedFailureGate;
}

namespace ao::rt::test::playback_succession
{
  struct PlaybackSuccessionSeekFixture final
  {
    explicit PlaybackSuccessionSeekFixture(audio::test::StagedFailureGate* failureGate = nullptr);
    ~PlaybackSuccessionSeekFixture();

    PlaybackSuccessionSeekFixture(PlaybackSuccessionSeekFixture const&) = delete;
    PlaybackSuccessionSeekFixture& operator=(PlaybackSuccessionSeekFixture const&) = delete;
    PlaybackSuccessionSeekFixture(PlaybackSuccessionSeekFixture&&) = delete;
    PlaybackSuccessionSeekFixture& operator=(PlaybackSuccessionSeekFixture&&) = delete;

    LibraryCommands& commands();
    TrackId addPlayableTrack(std::string title);
    void buildThreeTrackManualView();
    void buildSingleTrackManualView();
    Result<> playAndWait(TrackId trackId);

    MusicLibraryFixture libraryFixture;
    QueuedExecutor executor;
    async::Runtime asyncRuntime;
    LibraryChanges changes;
    LibraryCommandsFixture commandsFixture;
    TrackSourceCache sources;
    ViewService views;
    WorkspaceService workspace;
    NotificationService notifications{asyncRuntime};
    std::unique_ptr<PlaybackTransport> transportPtr;
    std::unique_ptr<PlaybackSuccession> successionPtr;
    TrackId firstTrackId = kInvalidTrackId;
    TrackId secondTrackId = kInvalidTrackId;
    TrackId thirdTrackId = kInvalidTrackId;
    ListId listId = kInvalidListId;
    ViewId viewId = kInvalidViewId;
    std::uint32_t nextPlayableFile = 0;
  };
} // namespace ao::rt::test::playback_succession
