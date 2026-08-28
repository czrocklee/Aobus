// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <cstdint>
#include <string_view>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  enum class TrackColumnSizing : std::uint8_t
  {
    Fixed,
    Flexible,
  };

  enum class TrackColumnAlignment : std::uint8_t
  {
    Start,
    End,
  };

  struct TrackColumnDefaults final
  {
    static constexpr std::int32_t kDefaultMinimumWidth = 40;

    std::int32_t width = -1;
    std::int32_t minimumWidth = kDefaultMinimumWidth;
    double weight = 1.0;
    TrackColumnSizing sizing = TrackColumnSizing::Fixed;
    TrackColumnAlignment alignment = TrackColumnAlignment::Start;

    bool operator==(TrackColumnDefaults const&) const = default;
  };

  TrackColumnDefaults trackColumnDefaults(rt::TrackField field) noexcept;

  std::string_view trackFieldColumnTitle(i18n::MessageCatalog const& textCatalog, rt::TrackField field);
} // namespace ao::uimodel
