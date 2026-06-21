// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/AlsaPcmError.h>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AlsaPcmError - classifies terminal stream errors", "[audio][unit][alsa]")
  {
    CHECK(isUnrecoverableAlsaPcmError(-ENODEV));
    CHECK(isUnrecoverableAlsaPcmError(-EBADF));

    CHECK_FALSE(isUnrecoverableAlsaPcmError(-EPIPE));
    CHECK_FALSE(isUnrecoverableAlsaPcmError(-ESTRPIPE));
    CHECK_FALSE(isUnrecoverableAlsaPcmError(-EAGAIN));
  }
} // namespace ao::audio::backend::detail::test
