// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackSuccession.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace ao::rt::test::playback_succession
{
  Result<> playFromViewAndWait(PlaybackSuccession& succession,
                               QueuedExecutor& executor,
                               ViewId viewId,
                               TrackId trackId);

  NavigationRequest navigationRequest(TrackListViewConfig config);

  struct PlaybackSuccessionFixture final
  {
    PlaybackSuccessionFixture();
    ~PlaybackSuccessionFixture();

    PlaybackSuccessionFixture(PlaybackSuccessionFixture const&) = delete;
    PlaybackSuccessionFixture& operator=(PlaybackSuccessionFixture const&) = delete;
    PlaybackSuccessionFixture(PlaybackSuccessionFixture&&) = delete;
    PlaybackSuccessionFixture& operator=(PlaybackSuccessionFixture&&) = delete;

    LibraryWriter& writer();
    TrackId addPlayableTrack(std::string title, std::uint16_t year = 2020);
    void removePlayableFile(TrackId trackId);
    void openManualView(std::span<TrackId const> trackIds, TrackListViewConfig config = {});
    void removeFromList(std::span<TrackId const> trackIds);
    Result<LibraryWriter::MoveOrderAuthoringResult> moveListOrder(std::span<TrackId const> selectedTrackIds,
                                                                  std::optional<TrackId> optBeforeTrackId);
    void buildThreeTrackManualView(TrackListViewConfig config = {});
    Result<> playAndWait(TrackId trackId);

    MusicLibraryFixture libraryFixture;
    ControlledSleeper sleeper;
    QueuedExecutor executor;
    InlineExecutor libraryChangesExecutor;
    async::Runtime asyncRuntime;
    LibraryChanges changes;
    LibraryWriterFixture writerFixture;
    TrackSourceCache sources;
    ViewService views;
    WorkspaceService workspace;
    NotificationService notifications{asyncRuntime};
    PlaybackTransport playbackTransport;
    std::unique_ptr<PlaybackSuccession> successionPtr;
    TrackId firstTrackId = kInvalidTrackId;
    TrackId secondTrackId = kInvalidTrackId;
    TrackId thirdTrackId = kInvalidTrackId;
    ListId listId = kInvalidListId;
    ViewId viewId = kInvalidViewId;
    std::uint32_t nextPlayableFile = 0;
  };
} // namespace ao::rt::test::playback_succession
