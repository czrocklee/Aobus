// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TerminalTrackColumnLayout.h"

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kSecondaryTextPreferredColumns = 14;
    constexpr std::int32_t kWideMetadataPreferredColumns = 18;
    constexpr std::int32_t kTimestampPreferredColumns = 12;

    std::int32_t terminalPreferredWidth(rt::TrackField const field) noexcept
    {
      using enum rt::TrackField;

      switch (field)
      {
        case Title: return 24;
        case Artist: return kSecondaryTextPreferredColumns;
        case Album: return kWideMetadataPreferredColumns;
        case AlbumArtist: return 16;
        case Genre:
        case Composer:
        case Conductor:
        case Ensemble:
        case Work:
        case Movement:
        case Soloist: return kSecondaryTextPreferredColumns;
        case Tags: return 16;
        case FilePath: return 32;
        case ModifiedTime: return kTimestampPreferredColumns;
        case TechnicalSummary: return kWideMetadataPreferredColumns;
        default: return kMinimumTrackColumnWidthColumns;
      }
    }

    std::int32_t trackColumnViewport(std::int32_t const availableColumns, std::size_t const columnCount) noexcept
    {
      if (availableColumns <= 0 || columnCount == 0)
      {
        return 0;
      }

      return std::max(0, availableColumns - trackTableChromeColumns(columnCount));
    }

    std::vector<rt::TrackField> visibleFields(rt::TrackPresentationSpec const& presentation,
                                              std::span<uimodel::TrackColumnState const> const storedLayout)
    {
      auto normalized = rt::normalizeTrackPresentationSpec(presentation);

      if (normalized.visibleFields.empty())
      {
        normalized = rt::defaultTrackPresentationSpec();
      }

      return uimodel::visibleTrackFieldsInStoredLayout(normalized.visibleFields, storedLayout);
    }

    std::vector<uimodel::TrackColumnSolveSpec> terminalSpecs(
      std::span<rt::TrackField const> const fields,
      std::span<uimodel::TrackColumnState const> const storedLayout)
    {
      auto specs = std::vector<uimodel::TrackColumnSolveSpec>{};
      specs.reserve(fields.size());

      for (auto const field : fields)
      {
        auto const defaults = uimodel::trackColumnDefaults(field);
        auto spec = uimodel::TrackColumnSolveSpec{
          .field = field,
          .weight = defaults.weight,
          .fixedWidth = -1,
          .defaultWidth = terminalPreferredWidth(field),
          .minimumWidth = kMinimumTrackColumnWidthColumns,
        };
        auto const stored = std::ranges::find(storedLayout, field, &uimodel::TrackColumnState::field);

        if (defaults.sizing == uimodel::TrackColumnSizing::Fixed)
        {
          if (stored != storedLayout.end() && stored->width > 0)
          {
            spec.fixedWidth =
              std::clamp(stored->width, kMinimumTrackColumnWidthColumns, kMaximumTrackColumnWidthColumns);
          }
        }
        else if (stored != storedLayout.end() && stored->weight > 0.0)
        {
          spec.weight = stored->weight;
        }

        specs.push_back(spec);
      }

      return specs;
    }
  } // namespace

  TerminalTrackColumnLayout projectTerminalTrackColumnLayout(
    rt::TrackPresentationSpec const& presentation,
    std::span<uimodel::TrackColumnState const> const storedLayout,
    std::int32_t const availableColumns)
  {
    auto const fields = visibleFields(presentation, storedLayout);
    auto const specs = terminalSpecs(fields, storedLayout);
    auto const widths = uimodel::solveTrackColumnWidths(specs, trackColumnViewport(availableColumns, fields.size()));
    auto layout = TerminalTrackColumnLayout{.availableColumns = availableColumns};
    layout.columns.reserve(fields.size());

    for (std::size_t index = 0; index < fields.size(); ++index)
    {
      layout.columns.push_back(TerminalTrackColumn{
        .field = fields[index],
        .columns = index < widths.size() ? widths[index] : terminalPreferredWidth(fields[index]),
      });
    }

    return layout;
  }

  std::vector<uimodel::TrackColumnState> resizeTerminalTrackColumnLayout(
    rt::TrackPresentationSpec const& presentation,
    std::span<uimodel::TrackColumnState const> const storedLayout,
    rt::TrackField const resizedField,
    std::int32_t const targetColumns,
    std::int32_t const availableColumns)
  {
    auto const fields = visibleFields(presentation, storedLayout);
    auto const specs = terminalSpecs(fields, storedLayout);
    auto const resizedSpecs = uimodel::resizeTrackColumnSpecs(
      specs,
      resizedField,
      std::clamp(targetColumns, kMinimumTrackColumnWidthColumns, kMaximumTrackColumnWidthColumns),
      trackColumnViewport(availableColumns, fields.size()));
    auto visibleLayout = std::vector<uimodel::TrackColumnState>{};
    visibleLayout.reserve(resizedSpecs.size());

    for (auto const& spec : resizedSpecs)
    {
      visibleLayout.push_back(uimodel::canonicalTrackColumnState(spec));
    }

    return uimodel::mergeVisibleTrackColumnLayout(storedLayout, visibleLayout);
  }
} // namespace ao::tui
