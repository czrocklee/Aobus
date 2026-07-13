// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <span>
#include <string>

namespace ao::media::mp4
{
  /**
   * Returns the first audio sample entry type, such as "alac" or "mp4a".
   * Returns NotFound when the container has no audio track and preserves
   * structural parse failures encountered before selection.
   */
  Result<std::string> audioSampleEntryType(std::span<std::byte const> fileData);
} // namespace ao::media::mp4
