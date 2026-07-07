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
  inline constexpr std::int32_t kPopupPanelBodyPaddingColumns = 1;

  ftxui::Decorator muted();
  ftxui::Decorator accent();
  ftxui::Decorator success();
  ftxui::Decorator warning();
  ftxui::Decorator danger();
  ftxui::Decorator interactiveSurface();
  ftxui::Decorator selected();
  ftxui::Decorator buttonHover();
  ftxui::Element shortcutChip(std::string_view key, std::string_view label);
  ftxui::Element mutedSeparator(std::string_view separator = " · ");
  ftxui::Element panelFooterHint(std::string_view hint);
  ftxui::Element statusSlot(ftxui::Element bodyPtr, std::int32_t minColumns = kClassicStatusSlotColumns);

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
  ftxui::Element titledPanel(std::string_view title, ftxui::Element bodyPtr, PanelOptions options = {});
  ftxui::Element popupPanel(std::string_view title, ftxui::Element bodyPtr, PanelOptions options = {});
} // namespace ao::tui::style
