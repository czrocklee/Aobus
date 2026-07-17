// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <string>

namespace ao::uimodel
{
  /** Formats a counted-track noun phrase such as "1 track" or "2 tracks". */
  std::string formatTrackCount(std::size_t count);
} // namespace ao::uimodel
