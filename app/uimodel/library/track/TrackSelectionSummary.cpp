// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackSelectionSummary.h>

#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace ao::uimodel
{
  std::string trackSelectionSummaryText(PresentationTextCatalog const& textCatalog,
                                        std::size_t const count,
                                        std::optional<std::chrono::milliseconds> const optTotalDuration)
  {
    auto duration = std::string{};

    if (optTotalDuration && *optTotalDuration > std::chrono::milliseconds{0})
    {
      duration = formatDuration(*optTotalDuration);
    }

    return textCatalog.trackSelectionSummary(count, duration);
  }
} // namespace ao::uimodel
