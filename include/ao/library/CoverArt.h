// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/PictureType.h>

namespace ao::library
{
  /**
   * CoverArt - typed cover art stored as a ResourceStore reference.
   */
  struct CoverArt
  {
    ResourceId resourceId{};
    PictureType type = PictureType::FrontCover;
  };
} // namespace ao::library
