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
} // namespace ao::tui
