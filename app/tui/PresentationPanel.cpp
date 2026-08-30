// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PresentationPanel.h"

#include "SelectableList.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include "TrackPresentationNavigation.h"
#include "TuiText.h"
#include <ao/i18n/MessageCatalog.h>

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

    std::string presentationPanelRowText(TrackPresentationNavEntry const& item)
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

  std::int32_t presentationPanelColumns(i18n::MessageCatalog const& textCatalog,
                                        std::vector<TrackPresentationNavEntry> const& items,
                                        std::string_view const activePresentationId,
                                        TuiKeymapPlan const& keymapPlan,
                                        std::int32_t const terminalColumns)
  {
    auto contentColumns = std::max(cellWidth(tuiChromeText(textCatalog, i18n::MessageId::TuiLibraryNoViewsAvailable)) +
                                     kPresentationPanelScrollIndicatorColumns,
                                   cellWidth(overlayHint(textCatalog, keymapPlan, Overlay::PresentationPanel)));
    contentColumns = std::max(contentColumns,
                              cellWidth(overlayLabel(textCatalog, Overlay::PresentationPanel)) + cellWidth(" · ") +
                                cellWidth(trackPresentationDisplayId(textCatalog, activePresentationId)));

    for (auto const& item : items)
    {
      contentColumns = std::max(contentColumns,
                                kPresentationPanelMarkerColumns + cellWidth(presentationPanelRowText(item)) +
                                  kPresentationPanelScrollIndicatorColumns);
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element presentationPanel(i18n::MessageCatalog const& textCatalog,
                                   std::vector<TrackPresentationNavEntry> const& items,
                                   std::string_view const activePresentationId,
                                   std::int32_t const selectedIndex,
                                   TuiKeymapPlan const& keymapPlan,
                                   std::vector<PresentationRowHitRegion>* const rowHitRegions,
                                   std::int32_t const columns)
  {
    using namespace ftxui;

    auto const panelColumns =
      columns <= 0 ? presentationPanelColumns(textCatalog, items, activePresentationId, keymapPlan, 0) : columns;

    auto rows = Elements{};
    auto listRows = std::vector<SelectableListRow>{};
    std::int32_t focusRow = 0;

    if (rowHitRegions != nullptr)
    {
      rowHitRegions->clear();
      rowHitRegions->reserve(items.size());
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

      if (rowHitRegions != nullptr)
      {
        rowHitRegions->push_back(PresentationRowHitRegion{.rowIndex = static_cast<std::int32_t>(index)});
        rowBox = &rowHitRegions->back().box;
      }

      listRows.push_back(SelectableListRow{.elementPtr = std::move(rowPtr), .selected = selected, .box = rowBox});
    }

    rows.push_back(selectableList(
      std::move(listRows),
      SelectableListOptions{.focusRow = focusRow,
                            .height = kPresentationPanelListRows,
                            .emptyText = tuiChromeText(textCatalog, i18n::MessageId::TuiLibraryNoViewsAvailable)}));
    rows.push_back(separator());
    rows.push_back(style::panelFooterHint(overlayHint(textCatalog, keymapPlan, Overlay::PresentationPanel)));

    auto activePresentationLabel = trackPresentationDisplayId(textCatalog, activePresentationId);
    return style::popupPanel(overlayLabel(textCatalog, Overlay::PresentationPanel),
                             vbox(std::move(rows)),
                             style::PanelOptions{.rightTitle = activePresentationLabel}) |
           size(WIDTH, EQUAL, panelColumns);
  }
} // namespace ao::tui
