// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackEditScript.h>

#include <concepts>
#include <type_traits>
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
  inline bool validateTrackSourceDelta(TrackSourceDelta const& message, std::size_t const initialSize) noexcept
  {
    return std::visit(
      [initialSize](auto const& value) noexcept
      {
        using Value = std::remove_cvref_t<decltype(value)>;

        if constexpr (std::same_as<Value, delta::RegularTrackEditScript>)
        {
          return !value.edits.empty() && delta::validate(value, initialSize);
        }
        else
        {
          return true;
        }
      },
      message);
  }
} // namespace ao::rt
