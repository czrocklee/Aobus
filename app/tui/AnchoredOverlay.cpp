// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AnchoredOverlay.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <utility>

namespace ao::tui
{
  namespace
  {
    bool isEmptyAnchor(ftxui::Box const& box)
    {
      return box.x_min == 0 && box.x_max == 0 && box.y_min == 0 && box.y_max == 0;
    }

    ftxui::Box bottomFallbackAnchor(AnchoredOverlayTerminal const terminal)
    {
      auto const row = std::max(0, terminal.rows - 1);
      return ftxui::Box{.x_min = 0, .x_max = std::max(0, terminal.columns - 1), .y_min = row, .y_max = row};
    }

    ftxui::Box layerAnchor(ftxui::Box const rootAnchor,
                           AnchoredOverlayTerminal const terminal,
                           AnchoredOverlayOptions const options)
    {
      auto anchor = options.fallbackToBottom && isEmptyAnchor(rootAnchor) ? bottomFallbackAnchor(terminal) : rootAnchor;
      anchor.y_min -= options.overlayLayerTopRows;
      anchor.y_max -= options.overlayLayerTopRows;
      return anchor;
    }
  } // namespace

  ftxui::Element anchoredOverlay(ftxui::Element overlayPtr,
                                 ftxui::Box const rootAnchor,
                                 AnchoredOverlayPlacement const placement,
                                 AnchoredOverlaySize const overlaySize,
                                 AnchoredOverlayTerminal const terminal,
                                 AnchoredOverlayOptions const options)
  {
    using namespace ftxui;

    auto const anchor = layerAnchor(rootAnchor, terminal, options);
    auto const maxLeft = terminal.columns > overlaySize.columns ? terminal.columns - overlaySize.columns : 0;
    auto const left = std::clamp(anchor.x_min, 0, maxLeft);
    auto const top = placement == AnchoredOverlayPlacement::Above ? std::max(0, anchor.y_min - overlaySize.rows)
                                                                  : std::max(0, anchor.y_max + 1);
    auto const rowsBelowPtr = placement == AnchoredOverlayPlacement::Above
                                ? filler() | size(HEIGHT, EQUAL, std::max(0, terminal.rows - top - overlaySize.rows))
                                : filler();

    return vbox({
      filler() | size(HEIGHT, EQUAL, top),
      hbox({
        filler() | size(WIDTH, EQUAL, left),
        std::move(overlayPtr) | clear_under,
        filler(),
      }),
      rowsBelowPtr,
    });
  }
} // namespace ao::tui
