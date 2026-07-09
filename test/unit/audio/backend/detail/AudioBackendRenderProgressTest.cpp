// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/AudioBackendRenderProgress.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AudioBackendRenderProgress - committedPositionFrames handles partial cross-boundary commits",
            "[audio][unit][backend][render]")
  {
    CHECK(committedPositionFrames(0, 2, 2) == 0);
    CHECK(committedPositionFrames(1, 2, 2) == 0);
    CHECK(committedPositionFrames(2, 2, 2) == 0);
    CHECK(committedPositionFrames(3, 2, 2) == 1);
    CHECK(committedPositionFrames(4, 2, 2) == 2);
    CHECK(committedPositionFrames(5, 2, 2) == 2);

    CHECK(committedPositionFrames(3, 0, 4) == 3);
  }
} // namespace ao::audio::backend::detail::test
