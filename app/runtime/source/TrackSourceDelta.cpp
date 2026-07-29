// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/source/TrackSourceDelta.h>

#include <ao/rt/TrackEditScript.h>

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <variant>

namespace ao::rt
{
  bool validateTrackSourceDelta(TrackSourceDelta const& message, std::size_t const initialSize) noexcept
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
