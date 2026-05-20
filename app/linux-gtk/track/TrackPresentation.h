// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/TrackField.h"

#include <sigc++/signal.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  static_assert(rt::kTrackFieldCount > 0, "rt::kTrackFieldCount must be positive");

  struct TrackColumnViewState final
  {
    std::array<std::int32_t, rt::kTrackFieldCount> widths{};
    std::vector<rt::TrackField> fieldOrder{};

    bool operator==(TrackColumnViewState const&) const = default;
  };

  std::int32_t defaultWidthForField(rt::TrackField field);
  bool fieldIsExpanding(rt::TrackField field);
  bool fieldIsVisibleByDefault(rt::TrackField field);
  std::string_view fieldColumnTitle(rt::TrackField field);

  std::optional<rt::TrackField> redundantFieldToColumn(rt::TrackSortField field);

  class TrackColumnLayoutModel final
  {
  public:
    using ChangedSignal = sigc::signal<void()>;

    explicit TrackColumnLayoutModel(TrackColumnViewState state = {});

    TrackColumnViewState const& state() const { return _state; }
    void setState(TrackColumnViewState const& state);
    void reset();

    ChangedSignal& signalChanged() { return _changed; }

  private:
    TrackColumnViewState _state;
    ChangedSignal _changed;
  };
} // namespace ao::gtk
