// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cerrno>
#include <cstdint>

namespace ao::audio::backend::detail
{
  inline bool isUnrecoverableAlsaPcmError(std::int32_t err) noexcept
  {
    return err == -ENODEV || err == -EBADF;
  }

  Error::Code alsaPcmOpenErrorCode(std::int32_t err) noexcept;
} // namespace ao::audio::backend::detail
