// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <string_view>

namespace ao::tui::style
{
  inline constexpr std::int32_t kClassicStatusSlotColumns = 24;
  inline constexpr std::int32_t kPanelBodyPaddingColumns = 1;

  ftxui::Decorator muted();
  ftxui::Decorator accent();
  ftxui::Decorator success();
  ftxui::Decorator warning();
  ftxui::Decorator danger();
  ftxui::Decorator interactiveSurface();
  ftxui::Decorator selected();
  ftxui::Decorator markedSurface();
  ftxui::Decorator buttonHover();
  ftxui::Element shortcutChip(std::string_view key, std::string_view label);
  ftxui::Element mutedSeparator(std::string_view separator = " · ");
  /// Clears the popover and a one-cell outer halo without enlarging its hit regions.
  ftxui::Element popoverClearHalo(ftxui::Element popoverPtr);

  ftxui::Element panelFooterHint(std::string_view hint);
  ftxui::Element statusSlot(ftxui::Element bodyPtr, std::int32_t minColumns = kClassicStatusSlotColumns);

  struct PanelDividerOptions final
  {
    bool separateBorders = false;
    ftxui::Box* hoverBox = nullptr;
    bool revealOnHover = false;
    bool dragging = false;
  };

  ftxui::Element panelIndicator(std::string_view arrow, ftxui::Box& box, bool hovered, bool onHoverOnly);

  ftxui::Element panelDivider(std::string_view arrow,
                              ftxui::Box* toggleBox,
                              bool hovered,
                              PanelDividerOptions options = {});

  struct PanelEdgeButton final
  {
    std::string_view label{};
    std::string_view value{};
    ftxui::Box* box = nullptr;
    bool hovered = false;
  };

  struct PanelOptions final
  {
    ftxui::Box* titleBox = nullptr;
    std::string_view rightTitle{};
    ftxui::Box* rightTitleBox = nullptr;
    PanelEdgeButton leftFooter{};
    PanelEdgeButton leftFooterRight{};
    std::string_view rightFooter{};
    std::int32_t bodyPaddingColumns = 0;
  };

  std::int32_t titledPanelColumnsForContent(std::int32_t contentColumns,
                                            std::int32_t terminalColumns,
                                            PanelOptions options = {});
  std::int32_t titledPanelBodyColumns(std::int32_t panelColumns, PanelOptions options = {});
  std::int32_t popupPanelColumnsForContent(std::int32_t contentColumns, std::int32_t terminalColumns);
  std::int32_t popupPanelBodyColumns(std::int32_t panelColumns);
  ftxui::Element panelBody(ftxui::Element bodyPtr);
  /// Pads a body whose built-in one-column scrollbar supplies its right gutter.
  /// Use inside titledPanel with no additional body padding.
  ftxui::Element scrollablePanelBody(ftxui::Element bodyPtr);
  ftxui::Element titledPanel(std::string_view title, ftxui::Element bodyPtr, PanelOptions options = {});
  ftxui::Element popupPanel(std::string_view title, ftxui::Element bodyPtr, PanelOptions options = {});
} // namespace ao::tui::style
