// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackSelectionSummary.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace ao::uimodel
{
  std::string trackSelectionSummaryText(i18n::MessageCatalog const& textCatalog,
                                        std::size_t const count,
                                        std::optional<std::chrono::milliseconds> const optTotalDuration)
  {
    auto duration = std::string{};

    if (optTotalDuration && *optTotalDuration > std::chrono::milliseconds{0})
    {
      duration = formatDuration(*optTotalDuration);
    }

    return trackSelectionSummary(textCatalog, count, duration);
  }
} // namespace ao::uimodel
