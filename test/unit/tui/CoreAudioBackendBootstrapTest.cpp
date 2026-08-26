// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "tui/AudioBackendBootstrap.h"
#include <ao/audio/BackendIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace ao::tui::test
{
  TEST_CASE("AudioBackendBootstrap - registers Core Audio with the TUI runtime", "[tui][unit][platform][coreaudio]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<rt::test::QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = rt::test::makeRuntime(tempDir, std::move(executorPtr));
    REQUIRE(runtimePtr->playback().snapshot().transport.output.availableBackends.empty());

    registerPlatformAudioBackends(*runtimePtr);
    executor->drain();

    auto const snapshot = runtimePtr->playback().snapshot();
    CHECK(std::ranges::any_of(snapshot.transport.output.availableBackends,
                              [](rt::OutputBackendSnapshot const& backend)
                              { return backend.id == audio::kBackendCoreAudio; }));
    runtimePtr->shutdown();
  }
} // namespace ao::tui::test
