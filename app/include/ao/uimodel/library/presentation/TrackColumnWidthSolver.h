// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <cstdint>
#include <span>
#include <vector>

namespace ao::uimodel
{
  struct TrackColumnSolveSpec final
  {
    rt::TrackField field = rt::TrackField::Title;
    double weight = -1.0;
    std::int32_t fixedWidth = -1;
    std::int32_t defaultWidth = 0;
    std::int32_t minimumWidth = 0;
  };

  std::vector<TrackColumnSolveSpec> pixelTrackColumnSpecs(std::span<rt::TrackField const> fields,
                                                          std::span<TrackColumnState const> storedLayout);

  std::vector<std::int32_t> solveTrackColumnWidths(std::span<TrackColumnSolveSpec const> specs,
                                                   std::int32_t viewportWidth);

  std::vector<TrackColumnSolveSpec> specsFromWidths(std::span<TrackColumnSolveSpec const> priorSpecs,
                                                    std::span<std::int32_t const> widths);

  std::vector<TrackColumnSolveSpec> resizeTrackColumnSpecs(std::span<TrackColumnSolveSpec const> specs,
                                                           rt::TrackField resizedField,
                                                           std::int32_t targetWidth,
                                                           std::int32_t viewportWidth);

  TrackColumnState canonicalTrackColumnState(TrackColumnSolveSpec const& spec);
} // namespace ao::uimodel
