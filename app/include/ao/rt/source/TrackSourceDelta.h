// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackEditScript.h>

#include <cstddef>
#include <variant>

namespace ao::rt
{
  struct SourceReset final
  {};

  struct SourceInvalidated final
  {};

  using TrackSourceDelta = std::variant<delta::RegularTrackEditScript, SourceReset, SourceInvalidated>;

  /**
   * Validates one source message against a source of initialSize rows.
   *
   * A regular script must be non-empty and canonical. Reset and Invalidated
   * carry no coordinate payload.
   */
  bool validateTrackSourceDelta(TrackSourceDelta const& message, std::size_t initialSize) noexcept;
} // namespace ao::rt
