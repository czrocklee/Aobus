// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Model.h"
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

  struct TrackSectionRowBox final
  {
    std::int32_t sectionIndex = -1;
    ftxui::Box box{};
  };

  struct TrackTableViewOptions final
  {
    std::vector<TrackColumnWidthOverride> const* columnWidths = nullptr;
    std::vector<TrackColumnResizeHandle>* resizeHandles = nullptr;
    std::vector<TrackSectionRowBox>* sectionRowBoxes = nullptr;
    ftxui::Box* tableBox = nullptr;
    std::int32_t availableColumns = 0;
  };

  std::int32_t trackVisualRow(std::int32_t trackIndex, std::span<TrackSection const> sections) noexcept;
  std::int32_t trackIndexForVisualRow(std::int32_t visualRow,
                                      std::size_t trackCount,
                                      std::span<TrackSection const> sections) noexcept;

  ftxui::Element trackTableView(std::span<TrackListItem const> tracks,
                                std::int32_t selected,
                                TrackId playingTrackId,
                                rt::TrackPresentationSpec const& presentation,
                                TrackTableViewOptions options = {});
  ftxui::Element trackTableView(std::span<TrackListItem const> tracks,
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
