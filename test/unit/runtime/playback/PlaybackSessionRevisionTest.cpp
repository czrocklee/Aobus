// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSessionRevision.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::rt::test
{
  TEST_CASE("PlaybackSessionRevision - acknowledges only the exact captured revision",
            "[runtime][unit][playback-session][revision]")
  {
    auto revision = PlaybackSessionRevision{};
    CHECK_FALSE(revision.dirty());
    CHECK(revision.markDirty());
    auto const captured = revision.capture();
    CHECK_FALSE(revision.markDirty());

    revision.acknowledge(captured);

    CHECK(revision.dirty());
    auto const newer = revision.capture();
    CHECK(newer > captured);
    revision.acknowledge(newer);
    CHECK_FALSE(revision.dirty());
  }

  TEST_CASE("PlaybackSessionRevision - remains dirty when a failed save does not acknowledge",
            "[runtime][unit][playback-session][revision]")
  {
    auto revision = PlaybackSessionRevision{};
    CHECK(revision.markDirty());
    [[maybe_unused]] auto const failedSaveCapture = revision.capture();

    CHECK(revision.dirty());
    CHECK_FALSE(revision.markDirty());
    CHECK(revision.dirty());
  }

  TEST_CASE("PlaybackSessionRevision - reset establishes a clean baseline without rewinding",
            "[runtime][unit][playback-session][revision]")
  {
    auto revision = PlaybackSessionRevision{};
    CHECK(revision.markDirty());
    auto const beforeReset = revision.capture();

    revision.resetClean();

    CHECK_FALSE(revision.dirty());
    CHECK(revision.capture() == beforeReset);
    CHECK(revision.markDirty());
    CHECK(revision.capture() > beforeReset);
  }
} // namespace ao::rt::test
