// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace ao::media::mp4
{
  /**
   * @brief Returns the first audio sample entry type from an MP4 stsd box, such as "alac" or "mp4a".
   */
  std::string audioSampleEntryType(std::span<std::byte const> fileData);
} // namespace ao::media::mp4
