// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/TestFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <memory>
#include <string_view>

namespace ao::library::test
{
  struct TrackSpec;
}

namespace ao::rt::test
{
  class QueuedExecutor;

  class PlaybackUiFixture final
  {
  public:
    PlaybackUiFixture();

    AppRuntime& runtime() const noexcept;
    QueuedExecutor& executor() const noexcept;
    ViewId viewId() const noexcept;

    void makePlaybackReady() const;

    TrackId addTrack(library::test::TrackSpec const& spec);
    TrackId addPlayableTrack(std::string_view title);

    Result<> playFromView(TrackId trackId);

    bool tryWaitForPlayback(TrackId trackId, PlaybackPositionRevision previousRevision) const;

  private:
    ao::test::TempDir _tempDir;
    QueuedExecutor* _executor = nullptr;
    std::unique_ptr<AppRuntimeOwner> _ownerPtr;
    ViewId _viewId{kInvalidViewId};
  };
} // namespace ao::rt::test
