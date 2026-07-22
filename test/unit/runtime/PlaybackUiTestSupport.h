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

#include <string>
#include <string_view>

namespace ao::rt::test
{
  struct PlaybackUiFixture final
  {
    PlaybackUiFixture()
      : runtime{makeRuntime(tempDir)}
      , viewId{ao::test::requireValue(runtime.workspace().navigate({.target = GlobalViewKind::AllTracks}))}
    {
    }

    void makePlaybackReady() { addReadyAudioProvider(runtime); }

    TrackId addPlayableTrack(std::string_view title)
    {
      auto const uri =
        audio::test::installAudioFixture(runtime.musicLibrary().rootPath(), "basic_metadata.flac", "ui-playable.flac");
      auto const trackId = addRuntimeTrack(runtime, {.title = std::string{title}, .uri = uri});
      runtime.reloadAllTracks();
      return trackId;
    }

    Result<> playFromView(TrackId trackId) { return runtime.playback().commands().startFromView(viewId, trackId); }

    // These fixture values are intentionally public as the tests' assertion surface.
    ao::test::TempDir tempDir;
    AppRuntime runtime;
    ViewId viewId{kInvalidViewId};
  };
} // namespace ao::rt::test
