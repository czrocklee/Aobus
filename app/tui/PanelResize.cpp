// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PanelResize.h"

#include "NavigationPanel.h"
#include "PanelWidths.h"

#include <algorithm>
#include <cstdint>

namespace ao::tui
{
  PanelResize::PanelResize(PanelWidths const widths,
                           NavigationGeometry const& geometry,
                           PanelDivider const divider) noexcept
    : _widths{widths}
    , _divider{divider}
    , _initialColumns{divider == PanelDivider::Detail ? geometry.detailColumns : geometry.columns}
    , _minimumColumns{std::min(_initialColumns,
                               divider == PanelDivider::Detail ? kMinimumDetailColumns : kMinimumNavigationColumns)}
    , _maximumColumns{_initialColumns + std::max(0, geometry.trackColumns - kMinimumTrackColumns)}
  {
  }

  PanelWidths PanelResize::moved(std::int32_t const columns) const noexcept
  {
    auto const delta = static_cast<std::int64_t>(columns) * (_divider == PanelDivider::Detail ? -1 : 1);
    auto const width =
      static_cast<std::int32_t>(std::clamp<std::int64_t>(_initialColumns + delta, _minimumColumns, _maximumColumns));
    auto widths = _widths;

    if (width != _initialColumns)
    {
      (_divider == PanelDivider::Detail ? widths.detail : widths.navigation) = width;
    }

    return widths;
  }
} // namespace ao::tui
