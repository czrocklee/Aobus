// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::gtk
{
  // A cell's demand on a shared row, not the path mode of an individual snapshot read.
  enum class TrackRowPathDemand : std::uint8_t
  {
    NoRequest, // Another bound cell can still require resolution on refresh.
    OnLoad,    // Include the path on load or refresh, but do not upgrade a fresh row.
    Required,  // Attempt resolution even for a fresh row that has not tried yet.
  };
} // namespace ao::gtk
