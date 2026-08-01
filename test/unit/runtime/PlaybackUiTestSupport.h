// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <memory>
#include <string_view>

namespace ao::rt::test
{
  struct PlaybackUiFixture final
  {
    PlaybackUiFixture();

    AppRuntime& runtime() const noexcept;

    void makePlaybackReady() const;

    TrackId addPlayableTrack(std::string_view title);

    Result<> playFromView(TrackId trackId);

    bool waitForPlayback(TrackId trackId);

    // These fixture values are intentionally public as the tests' assertion surface.
    ao::test::TempDir tempDir;
    std::unique_ptr<QueuedExecutor> executorOwnerPtr;
    QueuedExecutor* executor = nullptr;
    std::unique_ptr<AppRuntime> runtimePtr;
    ViewId viewId{kInvalidViewId};
    PlaybackPositionRevision observedPositionRevision{};
  };
} // namespace ao::rt::test
