// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/TrackRecord.h>

#include <span>

namespace rs::tag
{

  /**
   * ParsedTrack - Result of reading a single audio file's tags.
   *
   * This is the only output type returned by tag::File::loadTrack().
   * The embeddedCoverArt span references the mmap'd file data directly —
   * it is only valid for the lifetime of the owning File instance.
   */
  struct ParsedTrack final
  {
    rs::core::TrackRecord record;
    std::span<std::byte const> embeddedCoverArt;
  };

} // namespace rs::tag
