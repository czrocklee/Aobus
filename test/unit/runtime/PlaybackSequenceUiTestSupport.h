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
  class PlaybackSequenceUiFixture final
  {
  public:
    PlaybackSequenceUiFixture()
      : runtime{makeRuntime(tempDir)}
      , viewId{ao::test::requireValue(runtime.workspace().navigateTo(GlobalViewKind::AllTracks)).activeViewId}
    {
    }

    void makePlaybackReady() { addReadyAudioProvider(runtime.playback()); }

    TrackId addPlayableTrack(std::string_view title)
    {
      auto const path = audio::test::requireAudioFixture("basic_metadata.flac").string();
      auto const trackId = library::test::addTrack(runtime.musicLibrary(), {.title = std::string{title}, .uri = path});
      runtime.reloadAllTracks();
      return trackId;
    }

    Result<> playFromView(TrackId trackId) { return runtime.playbackSequence().playFromView(viewId, trackId); }

    // These fixture values are intentionally public as the tests' assertion surface.
    // NOLINTBEGIN(aobus-readability-identifier-naming-extensions)
    ao::test::TempDir tempDir;
    AppRuntime runtime;
    ViewId viewId{kInvalidViewId};
    // NOLINTEND(aobus-readability-identifier-naming-extensions)
  };
} // namespace ao::rt::test
