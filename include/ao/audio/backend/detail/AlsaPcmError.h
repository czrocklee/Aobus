// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cerrno>
#include <cstdint>

namespace ao::audio::backend::detail
{
  inline bool isUnrecoverableAlsaPcmError(std::int32_t err) noexcept
  {
    return err == -ENODEV || err == -EBADF;
  }
} // namespace ao::audio::backend::detail
