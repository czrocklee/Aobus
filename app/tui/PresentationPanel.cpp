// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PresentationPanel.h"

#include "Model.h"
#include "SelectableList.h"
#include "ShellModel.h"
#include "Style.h"
#include "TextCell.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kPresentationPanelMarkerColumns = 2;
    constexpr std::int32_t kPresentationPanelScrollIndicatorColumns = 1;

    std::string presentationPanelRowText(PresentationNavItem const& item)
    {
      auto label = item.label;

      if (!item.detail.empty())
      {
        label.append(" - ");
        label.append(item.detail);
      }

      return label;
    }
  } // namespace

  std::int32_t presentationPanelColumns(std::vector<PresentationNavItem> const& items,
                                        std::string_view const activePresentationId,
                                        std::int32_t const terminalColumns)
  {
    auto contentColumns = std::max(cellWidth("No views available") + kPresentationPanelScrollIndicatorColumns,
                                   cellWidth(overlayHint(Overlay::PresentationPanel)));
    contentColumns = std::max(
      contentColumns, cellWidth("Views") + cellWidth(" · ") + cellWidth(presentationDisplayId(activePresentationId)));

    for (auto const& item : items)
    {
      contentColumns = std::max(contentColumns,
                                kPresentationPanelMarkerColumns + cellWidth(presentationPanelRowText(item)) +
                                  kPresentationPanelScrollIndicatorColumns);
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element presentationPanel(std::vector<PresentationNavItem> const& items,
                                   std::string_view const activePresentationId,
                                   std::int32_t const selectedIndex,
                                   std::vector<PresentationRowBox>* const rowBoxes,
                                   std::int32_t const columns)
  {
    using namespace ftxui;

    auto const panelColumns = columns <= 0 ? presentationPanelColumns(items, activePresentationId, 0) : columns;

    auto rows = Elements{};
    auto listRows = std::vector<SelectableListRow>{};
    std::int32_t focusRow = 0;

    if (rowBoxes != nullptr)
    {
      rowBoxes->clear();
      rowBoxes->reserve(items.size());
    }

    listRows.reserve(items.size());

    for (std::size_t index = 0; index < items.size(); ++index)
    {
      auto const& item = items[index];
      auto label = presentationPanelRowText(item);

      auto rowPtr = hbox({
        text(item.id == activePresentationId ? "* " : "  "),
        text(std::move(label)) | flex,
      });

      auto const selected = std::cmp_equal(index, selectedIndex);
      auto* rowBox = static_cast<ftxui::Box*>(nullptr);

      if (selected)
      {
        focusRow = static_cast<std::int32_t>(listRows.size());
      }

      if (rowBoxes != nullptr)
      {
        rowBoxes->push_back(PresentationRowBox{.rowIndex = static_cast<std::int32_t>(index)});
        rowBox = &rowBoxes->back().box;
      }

      listRows.push_back(SelectableListRow{.elementPtr = std::move(rowPtr), .selected = selected, .box = rowBox});
    }

    rows.push_back(
      selectableList(std::move(listRows),
                     SelectableListOptions{
                       .focusRow = focusRow, .height = kPresentationPanelListRows, .emptyText = "No views available"}));
    rows.push_back(separator());
    rows.push_back(style::panelFooterHint(overlayHint(Overlay::PresentationPanel)));

    auto activePresentationLabel = presentationDisplayId(activePresentationId);
    return style::popupPanel(
             "Views", vbox(std::move(rows)), style::PanelOptions{.rightTitle = activePresentationLabel}) |
           size(WIDTH, EQUAL, panelColumns);
  }
} // namespace ao::tui
