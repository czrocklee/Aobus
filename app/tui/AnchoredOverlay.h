// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <memory>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  enum class AnchoredOverlayPlacement : std::uint8_t
  {
    Below,
    Above,
  };

  struct AnchoredOverlaySize final
  {
    std::int32_t columns = 0;
    std::int32_t rows = 0;
  };

  struct AnchoredOverlayTerminal final
  {
    std::int32_t columns = 0;
    std::int32_t rows = 0;
  };

  struct AnchoredOverlayOptions final
  {
    std::int32_t overlayLayerTopRows = 0;
    bool fallbackToBottom = false;
  };

  ftxui::Element anchoredOverlay(ftxui::Element overlayPtr,
                                 ftxui::Box rootAnchor,
                                 AnchoredOverlayPlacement placement,
                                 AnchoredOverlaySize overlaySize,
                                 AnchoredOverlayTerminal terminal,
                                 AnchoredOverlayOptions options = {});
  /// Reads a frame button's anchor during layout, after the background placed it.
  /// The anchor must outlive the returned element and be reflected during layout.
  ftxui::Element followingAnchoredOverlay(ftxui::Element overlayPtr,
                                          ftxui::Box const& rootAnchor,
                                          AnchoredOverlayPlacement placement,
                                          AnchoredOverlaySize overlaySize,
                                          AnchoredOverlayTerminal terminal,
                                          AnchoredOverlayOptions options = {});
  ftxui::Element followingAnchoredOverlay(ftxui::Element overlayPtr,
                                          ftxui::Box const&& rootAnchor,
                                          AnchoredOverlayPlacement placement,
                                          AnchoredOverlaySize overlaySize,
                                          AnchoredOverlayTerminal terminal,
                                          AnchoredOverlayOptions options = {}) = delete;
} // namespace ao::tui
