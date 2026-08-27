// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CoverArt.h"
#include "TrackListEntry.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  using KittyEscapeSink = std::function<void(std::string_view)>;
  void defaultKittyEscapeSink(std::string_view escapeSequence);

  /**
   * @brief The artwork slot Detail shows for the current cover state.
   *
   * A published transform becomes bare artwork: the block image itself, or an
   * empty reservation of the same cells for the image Kitty paints out of
   * band. Anything else becomes one compact unavailable line, so a selection
   * without artwork costs a row rather than a panel.
   *
   * @p columns is the session-wide artwork width every delivery mode claims,
   * so a mode switch cannot move the surrounding layout. It is mandatory
   * because it must be the same value the loader decoded against and the same
   * value @ref detailPaneColumns reserved the pane for.
   *
   * @p optArtworkBox is cleared first and reflected only by a reservation, so a
   * frame that shows no artwork leaves an invalid box behind for out-of-band
   * paint state to recognize.
   */
  ftxui::Element detailCoverArt(i18n::MessageCatalog const& textCatalog,
                                CoverArtDeliveryMode mode,
                                std::optional<CoverArtRows> const& optPreview,
                                std::optional<std::vector<std::byte>> const& optKittyPng,
                                std::int32_t columns,
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
  /// The escape drawing @p png into @p coverBox, empty when the box reserves no cells.
  std::string kittyCoverArtPaintEscape(ftxui::Box const& coverBox, std::vector<std::byte> const& png);

  struct KittyPaintState final
  {
    bool visible = false;
    ResourceId paintedCoverArtId = kInvalidResourceId;
    ftxui::Box paintedCoverBox{};
  };

  bool isValidBox(ftxui::Box const& box);
  bool isSameBox(ftxui::Box const& left, ftxui::Box const& right);
  bool isSameKittyImage(KittyPaintState const& state, ResourceId coverArtId, ftxui::Box const& coverBox);

  /**
   * @brief Brings the out-of-band Kitty image in line with the drawn frame.
   *
   * The frame reflects a box only when it actually reserved artwork cells, so
   * an invalid box is how a frame says the image has no place any more and
   * the terminal must be told to drop it.
   *
   * An unchanged image emits nothing. A replacement emits its delete and its
   * draw as one sink write bracketed by a synchronized update, so a terminal
   * honoring that mode cannot render the moment between them; one that ignores
   * the mode falls back to the ordinary two-escape sequence.
   */
  void updateKittyCoverArt(KittyPaintState& state,
                           ResourceId cachedCoverArtId,
                           ftxui::Box const& coverBox,
                           std::optional<std::vector<std::byte>> const& optKittyCoverArtPng,
                           KittyEscapeSink const& sink = defaultKittyEscapeSink);

  ftxui::Element centerPopover(ftxui::Element popoverPtr);
  std::int32_t detailPaneColumns(i18n::MessageCatalog const& textCatalog,
                                 std::int32_t terminalColumns,
                                 std::int32_t coverColumns);
  ftxui::Element detailPane(i18n::MessageCatalog const& textCatalog,
                            TrackListEntry const* selectedTrack,
                            ftxui::Element coverElementPtr,
                            std::int32_t columns);
  ftxui::Element helpPane(i18n::MessageCatalog const& textCatalog, std::int32_t terminalColumns = 0);
} // namespace ao::tui
