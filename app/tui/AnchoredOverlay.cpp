// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AnchoredOverlay.h"

#include "Style.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace ao::tui
{
  namespace
  {
    class FollowingOverlayNode final : public ftxui::Node
    {
    public:
      FollowingOverlayNode(ftxui::Element overlayPtr,
                           ftxui::Box const& anchor,
                           AnchoredOverlayPlacement const placement,
                           AnchoredOverlaySize const size,
                           AnchoredOverlayTerminal const terminal,
                           AnchoredOverlayOptions const options)
        : Node{{overlayPtr}}
        , _overlayPtr{std::move(overlayPtr)}
        , _anchor{anchor}
        , _placement{placement}
        , _size{size}
        , _terminal{terminal}
        , _options{options}
      {
      }

      void ComputeRequirement() override
      {
        _overlayPtr->ComputeRequirement();
        requirement_ = _overlayPtr->requirement();
      }

      void SetBox(ftxui::Box const box) override
      {
        Node::SetBox(box);
        children_.front() = anchoredOverlay(_overlayPtr, _anchor, _placement, _size, _terminal, _options);
        children_.front()->ComputeRequirement();
        children_.front()->SetBox(box);
      }

    private:
      ftxui::Element _overlayPtr;
      ftxui::Box const& _anchor;
      AnchoredOverlayPlacement _placement;
      AnchoredOverlaySize _size;
      AnchoredOverlayTerminal _terminal;
      AnchoredOverlayOptions _options;
    };

    bool isEmptyAnchor(ftxui::Box const& box)
    {
      return box.IsEmpty() || (box.x_min == 0 && box.x_max == 0 && box.y_min == 0 && box.y_max == 0);
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
    auto const outerColumns = overlaySize.columns + 2;
    auto const outerRows = overlaySize.rows + 2;
    auto const maxLeft = std::max(0, terminal.columns - outerColumns);
    auto const left = std::clamp(anchor.x_min - 1, 0, maxLeft);
    auto const top = placement == AnchoredOverlayPlacement::Above ? std::max(0, anchor.y_min - outerRows)
                                                                  : std::max(0, anchor.y_max + 1);
    auto const rowsBelowPtr = placement == AnchoredOverlayPlacement::Above
                                ? filler() | size(HEIGHT, EQUAL, std::max(0, terminal.rows - top - outerRows))
                                : filler();

    return vbox({
      filler() | size(HEIGHT, EQUAL, top),
      hbox({
        filler() | size(WIDTH, EQUAL, left),
        style::popoverClearHalo(std::move(overlayPtr)),
        filler(),
      }),
      rowsBelowPtr,
    });
  }

  ftxui::Element followingAnchoredOverlay(ftxui::Element overlayPtr,
                                          ftxui::Box const& rootAnchor,
                                          AnchoredOverlayPlacement const placement,
                                          AnchoredOverlaySize const overlaySize,
                                          AnchoredOverlayTerminal const terminal,
                                          AnchoredOverlayOptions const options)
  {
    return std::make_shared<FollowingOverlayNode>(
      std::move(overlayPtr), rootAnchor, placement, overlaySize, terminal, options);
  }
} // namespace ao::tui
