// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/WorkspaceService.h>

#include <string>
#include <string_view>

namespace ao::rt::test
{
  struct PlaybackSequenceUiFixture final
  {
    PlaybackSequenceUiFixture()
      : runtime{makeRuntime(tempDir)}
      , viewId{ao::test::requireValue(runtime.workspace().navigateTo(GlobalViewKind::AllTracks)).activeViewId}
    {
    }

    void makePlaybackReady() { addReadyAudioProvider(runtime.playback()); }

    TrackId addPlayableTrack(std::string_view title)
    {
      auto const uri =
        audio::test::installAudioFixture(runtime.musicLibrary().rootPath(), "basic_metadata.flac", "ui-playable.flac");
      auto const trackId = addRuntimeTrack(runtime, {.title = std::string{title}, .uri = uri});
      runtime.reloadAllTracks();
      return trackId;
    }

    Result<> playFromView(TrackId trackId) { return runtime.playbackSequence().playFromView(viewId, trackId); }

    // These fixture values are intentionally public as the tests' assertion surface.
    ao::test::TempDir tempDir;
    AppRuntime runtime;
    ViewId viewId{kInvalidViewId};
  };
} // namespace ao::rt::test
