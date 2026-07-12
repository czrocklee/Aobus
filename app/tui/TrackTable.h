// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackListEntry.h"
#include "TrackSection.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  inline constexpr std::int32_t kMinimumTrackColumnWidthColumns = 8;
  inline constexpr std::int32_t kMaximumTrackColumnResizeColumns = 160;

  struct TrackColumnWidthOverride final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::int32_t columns = 0;
  };

  struct TrackColumnResizeHandle final
  {
    rt::TrackField field = rt::TrackField::Title;
    ftxui::Box box{};
    std::int32_t columns = 0;
  };

  struct TrackSectionRowHitRegion final
  {
    std::int32_t sectionIndex = -1;
    ftxui::Box box{};
  };

  struct TrackTableViewOptions final
  {
    std::vector<TrackColumnWidthOverride> const* columnWidths = nullptr;
    std::vector<TrackColumnResizeHandle>* resizeHandles = nullptr;
    std::vector<TrackSectionRowHitRegion>* sectionRowHitRegions = nullptr;
    ftxui::Box* tableBox = nullptr;
    std::int32_t availableColumns = 0;
    // Upper bound on the visible viewport height. 0 disables windowing and builds
    // every row for full-build comparisons.
    std::int32_t viewportRows = 0;
  };

  // Overscan rows padded onto each side of the virtualized track-table window so
  // off-by-one at the viewport edges never reveals an unbuilt row.
  inline constexpr std::int32_t kTrackTableOverscanRows = 8;

  struct TrackTableWindow final
  {
    std::int32_t startVisualRow = 0;   // inclusive
    std::int32_t endVisualRow = 0;     // exclusive
    std::int32_t topSpacerRows = 0;    // == startVisualRow
    std::int32_t bottomSpacerRows = 0; // == totalVisualRows - endVisualRow
  };

  // Selects the window of visual rows to build around the selected row.
  // viewportRows <= 0 selects the whole range (no windowing).
  TrackTableWindow computeTrackTableWindow(std::int32_t selectedVisualRow,
                                           std::int32_t totalVisualRows,
                                           std::int32_t viewportRows,
                                           std::int32_t overscanRows) noexcept;

  struct TrackTableRowRef final
  {
    bool isSectionHeader = false;
    std::size_t sectionIndex = 0; // valid when isSectionHeader
    std::size_t trackIndex = 0;   // valid otherwise
  };

  // Enumerates the section-header / track rows occupying visual rows
  // [startVisualRow, endVisualRow), reproducing trackTableView's header
  // interleaving exactly but only for the requested range.
  std::vector<TrackTableRowRef> enumerateTrackTableRows(std::span<TrackSection const> sections,
                                                        std::size_t trackCount,
                                                        std::int32_t startVisualRow,
                                                        std::int32_t endVisualRow);

  std::int32_t trackVisualRow(std::int32_t trackIndex, std::span<TrackSection const> sections) noexcept;
  std::int32_t trackIndexForVisualRow(std::int32_t visualRow,
                                      std::size_t trackCount,
                                      std::span<TrackSection const> sections) noexcept;

  ftxui::Element trackTableView(std::span<TrackListEntry const> tracks,
                                std::int32_t selected,
                                TrackId playingTrackId,
                                rt::TrackPresentationSpec const& presentation,
                                TrackTableViewOptions options = {});
  ftxui::Element trackTableView(std::span<TrackListEntry const> tracks,
                                std::span<TrackSection const> sections,
                                std::int32_t selected,
                                TrackId playingTrackId,
                                rt::TrackPresentationSpec const& presentation,
                                TrackTableViewOptions options = {});
  std::int32_t libraryChooserPaneColumns(std::vector<std::string> const& labels, std::int32_t terminalColumns);
  ftxui::Element libraryChooserPane(std::vector<std::string> const& labels,
                                    std::int32_t selected,
                                    std::int32_t columns = 0);
} // namespace ao::tui
