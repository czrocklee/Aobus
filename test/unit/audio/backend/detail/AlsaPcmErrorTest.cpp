// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaPcmError.h"

#include <ao/Error.h>

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

  TEST_CASE("AlsaPcmError - reports device contention distinctly", "[audio][unit][alsa]")
  {
    CHECK(alsaPcmOpenErrorCode(-EBUSY) == Error::Code::ResourceBusy);
    CHECK(alsaPcmOpenErrorCode(-ENODEV) == Error::Code::DeviceNotFound);
    CHECK(alsaPcmOpenErrorCode(-ENOENT) == Error::Code::DeviceNotFound);
    CHECK(alsaPcmOpenErrorCode(-ENXIO) == Error::Code::DeviceNotFound);
    CHECK(alsaPcmOpenErrorCode(-ENOMEM) == Error::Code::ResourceExhausted);
    CHECK(alsaPcmOpenErrorCode(-EACCES) == Error::Code::InitFailed);
    CHECK(alsaPcmOpenErrorCode(-EINVAL) == Error::Code::InitFailed);
  }
} // namespace ao::audio::backend::detail::test
