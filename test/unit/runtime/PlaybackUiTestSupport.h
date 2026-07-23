// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>

#include <memory>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  struct PlaybackUiFixture final
  {
    PlaybackUiFixture()
      : executorOwnerPtr{std::make_unique<QueuedExecutor>()}
      , executor{executorOwnerPtr.get()}
      , runtime{makeRuntime(tempDir, std::move(executorOwnerPtr))}
      , viewId{ao::test::requireValue(runtime.workspace().navigate({.target = GlobalViewKind::AllTracks}))}
    {
    }

    void makePlaybackReady()
    {
      addReadyAudioProvider(runtime);
      executor->drain();
    }

    TrackId addPlayableTrack(std::string_view title)
    {
      auto const uri =
        audio::test::installAudioFixture(runtime.musicLibrary().rootPath(), "basic_metadata.flac", "ui-playable.flac");
      auto const trackId =
        addRuntimeTrack(runtime, {.title = std::string{title}, .uri = uri}, [this] { executor->drain(); });
      runtime.reloadAllTracks();
      return trackId;
    }

    Result<> playFromView(TrackId trackId)
    {
      auto admitted = admitPlaybackAndWait(
        *executor,
        [this, trackId] { return runtime.playback().commands().startFromView(viewId, trackId); },
        [this] { return runtime.playback().snapshot().transport.positionRevision; });

      if (admitted)
      {
        observedPositionRevision = runtime.playback().snapshot().transport.positionRevision;
      }

      return admitted;
    }

    bool waitForPlayback(TrackId trackId)
    {
      auto const settled =
        waitForPlaybackSettlement(*executor,
                                  observedPositionRevision,
                                  [this] { return runtime.playback().snapshot().transport.positionRevision; });
      observedPositionRevision = runtime.playback().snapshot().transport.positionRevision;
      return settled && runtime.playback().snapshot().transport.nowPlaying.trackId == trackId;
    }

    // These fixture values are intentionally public as the tests' assertion surface.
    ao::test::TempDir tempDir;
    std::unique_ptr<QueuedExecutor> executorOwnerPtr;
    QueuedExecutor* executor = nullptr;
    AppRuntime runtime;
    ViewId viewId{kInvalidViewId};
    PlaybackPositionRevision observedPositionRevision{};
  };
} // namespace ao::rt::test
