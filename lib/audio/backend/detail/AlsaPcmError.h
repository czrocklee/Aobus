// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>

#include <cerrno>
#include <cstdint>

namespace ao::audio::backend::detail
{
  inline bool isUnrecoverableAlsaPcmError(std::int32_t err) noexcept
  {
    return err == -ENODEV || err == -EBADF;
  }

  Error::Code alsaPcmOpenErrorCode(std::int32_t err) noexcept;

  // Only pre-admission exact rate/count constraints and format probes use
  // -EINVAL as an empty intersection. Other failures remain OutputFailure.
  // Do not use this for PCM open, applying an admitted mode or read-back:
  // those operations retain their own conservative failure policy.
  BackendOpenFailure::Cause alsaHwConstraintFailureCause(std::int32_t status) noexcept;
} // namespace ao::audio::backend::detail
