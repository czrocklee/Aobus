// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Type.h>

#include <string>
#include <vector>

namespace rs::core
{

  /**
   * ListRecord - Pure domain model for list data.
   *
   * All fields are stored directly. Serialization happens in ListBuilder.
   */
  class ListRecord
  {
  public:
    ListRecord() = default;

    /**
     * List name.
     */
    std::string name;

    /**
     * List description.
     */
    std::string description;

    /**
     * Filter expression for smart lists.
     * Empty string means manual list (uses trackIds instead).
     */
    std::string filter;

    /**
     * Track IDs for manual lists.
     * Empty for smart lists (uses filter instead).
     */
    std::vector<TrackId> trackIds;
  };

} // namespace rs::core
