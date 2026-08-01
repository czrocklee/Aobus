// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/PlaybackUiTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  PlaybackUiFixture::PlaybackUiFixture()
    : executorOwnerPtr{std::make_unique<QueuedExecutor>()}
    , executor{executorOwnerPtr.get()}
    , runtimePtr{makeRuntime(tempDir, std::move(executorOwnerPtr))}
    , viewId{ao::test::requireValue(runtime().workspace().navigate({.target = GlobalViewKind::AllTracks}))}
  {
  }

  AppRuntime& PlaybackUiFixture::runtime() const noexcept
  {
    return *runtimePtr;
  }

  void PlaybackUiFixture::makePlaybackReady() const
  {
    addReadyAudioProvider(runtime());
    executor->drain();
  }

  TrackId PlaybackUiFixture::addPlayableTrack(std::string_view const title)
  {
    auto const uri =
      audio::test::installAudioFixture(runtime().musicLibrary().rootPath(), "basic_metadata.flac", "ui-playable.flac");
    auto const trackId =
      addRuntimeTrack(runtime(), {.title = std::string{title}, .uri = uri}, [this] { executor->drain(); });
    runtime().reloadAllTracks();
    return trackId;
  }

  Result<> PlaybackUiFixture::playFromView(TrackId const trackId)
  {
    auto admitted = admitPlaybackAndWait(
      *executor,
      [this, trackId] { return runtime().playback().commands().startFromView(viewId, trackId); },
      [this] { return runtime().playback().snapshot().transport.positionRevision; });

    if (admitted)
    {
      observedPositionRevision = runtime().playback().snapshot().transport.positionRevision;
    }

    return admitted;
  }

  bool PlaybackUiFixture::waitForPlayback(TrackId const trackId)
  {
    auto const settled =
      waitForPlaybackSettlement(*executor,
                                observedPositionRevision,
                                [this] { return runtime().playback().snapshot().transport.positionRevision; });
    observedPositionRevision = runtime().playback().snapshot().transport.positionRevision;
    return settled && runtime().playback().snapshot().transport.nowPlaying.trackId == trackId;
  }
} // namespace ao::rt::test
