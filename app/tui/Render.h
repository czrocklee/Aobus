// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CoverArt.h"
#include "TrackListEntry.h"

#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  class TuiTextCatalog;
} // namespace ao::tui
namespace ao::uimodel
{
  class PresentationTextCatalog;
} // namespace ao::uimodel
namespace ao::tui
{
  /**
   * @brief The artwork slot Detail shows for the current cover state.
   *
   * A published transform becomes bare artwork: the block image itself, or an
   * empty reservation of the same cells for the image Kitty paints out of
   * band. Anything else becomes one compact unavailable line, so a selection
   * without artwork costs a row rather than a panel.
   *
   * @p optArtworkBox is cleared first and reflected only by a reservation, so a
   * frame that shows no artwork leaves an invalid box behind for out-of-band
   * paint state to recognize.
   */
  ftxui::Element detailCoverArt(uimodel::PresentationTextCatalog const& textCatalog,
                                CoverArtDeliveryMode mode,
                                std::optional<CoverArtRows> const& optPreview,
                                std::optional<std::vector<std::byte>> const& optKittyPng,
                                ftxui::Box* optArtworkBox = nullptr);
  /**
   * @brief Whether artwork fits beside worst-case metadata in @p availableRows.
   *
   * Measured against every field Detail can show rather than the rows one
   * track produces, so artwork cannot appear and disappear as the selection
   * moves between sparse and fully tagged tracks. Metadata wins the short
   * terminal.
   */
  bool detailPaneShowsCoverArt(std::int32_t availableRows);
  void paintKittyCoverArt(ftxui::Box const& coverBox, std::vector<std::byte> const& png);

  ftxui::Element centerPopover(ftxui::Element popoverPtr);
  std::int32_t detailPaneColumns(uimodel::PresentationTextCatalog const& textCatalog, std::int32_t terminalColumns);
  ftxui::Element detailPane(uimodel::PresentationTextCatalog const& textCatalog,
                            TrackListEntry const* selectedTrack,
                            ftxui::Element coverElementPtr,
                            std::int32_t columns = 0);
  std::int32_t helpPaneColumns(TuiTextCatalog const& textCatalog, std::int32_t terminalColumns);
  ftxui::Element helpPane(TuiTextCatalog const& textCatalog, std::int32_t columns = 0);
} // namespace ao::tui
