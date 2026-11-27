// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/runtime/PlaybackUiTestSupport.h"

#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  PlaybackUiFixture::PlaybackUiFixture()
  {
    auto executorPtr = std::make_unique<QueuedExecutor>();
    _executor = executorPtr.get();
    _ownerPtr = makeRuntime(_tempDir, std::move(executorPtr));
    _viewId = ao::test::requireValue(runtime().workspace().navigate({.target = GlobalViewKind::AllTracks}));
  }

  AppRuntime& PlaybackUiFixture::runtime() const noexcept
  {
    return _ownerPtr->runtime();
  }

  QueuedExecutor& PlaybackUiFixture::executor() const noexcept
  {
    return *_executor;
  }

  ViewId PlaybackUiFixture::viewId() const noexcept
  {
    return _viewId;
  }

  void PlaybackUiFixture::makePlaybackReady() const
  {
    addReadyAudioProvider(runtime());
    _executor->drain();
  }

  TrackId PlaybackUiFixture::addTrack(library::test::TrackSpec const& spec)
  {
    auto const trackId = addRuntimeTrack(*_ownerPtr, spec, [this] { _executor->drain(); });
    runtime().sources().reloadAllTracks();
    return trackId;
  }

  TrackId PlaybackUiFixture::addPlayableTrack(std::string_view const title)
  {
    auto const uri = audio::test::installAudioFixture(runtime().musicRoot(), "basic_metadata.flac", "ui-playable.flac");
    return addTrack({.title = std::string{title}, .uri = uri});
  }

  Result<> PlaybackUiFixture::playFromView(TrackId const trackId)
  {
    return admitPlaybackAndWait(
      *_executor,
      [this, trackId] { return runtime().playback().commands().startFromView(_viewId, trackId); },
      [this] { return runtime().playback().snapshot().transport.positionRevision; });
  }

  bool PlaybackUiFixture::tryWaitForPlayback(TrackId const trackId,
                                             PlaybackPositionRevision const previousRevision) const
  {
    auto const& playback = runtime().playback();
    auto const settled = tryWaitForPlaybackSettlement(
      *_executor, previousRevision, [&playback] { return playback.snapshot().transport.positionRevision; });
    return settled && playback.snapshot().transport.nowPlaying.trackId == trackId;
  }
} // namespace ao::rt::test
