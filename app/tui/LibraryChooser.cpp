// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryChooser.h"

#include "Keymap.h"
#include "LibraryNavigation.h"
#include "ListSearch.h"
#include "SelectableList.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  std::int32_t libraryChooserPaneColumns(i18n::MessageCatalog const& textCatalog,
                                         std::vector<std::string> const& labels,
                                         KeymapPlan const& keymapPlan,
                                         std::int32_t const terminalColumns)
  {
    auto contentColumns = std::max({cellWidth(overlayLabel(textCatalog, Overlay::ListChooser)),
                                    cellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TuiLibraryNoListsFound)),
                                    cellWidth(overlayHint(textCatalog, keymapPlan, Overlay::ListChooser))});

    for (auto const& label : labels)
    {
      contentColumns = std::max(contentColumns, cellWidth(label));
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element libraryChooserPane(i18n::MessageCatalog const& textCatalog,
                                    std::vector<std::string> const& labels,
                                    std::vector<LibraryNavEntry> const& items,
                                    std::int32_t const selected,
                                    KeymapPlan const& keymapPlan,
                                    std::int32_t columns,
                                    ListSearch const& search,
                                    std::vector<LibraryRowHitRegion>& rowHitRegions,
                                    ftxui::Box& viewportBox)
  {
    using namespace ftxui;
    AO_EXPECTS(labels.size() == items.size(), "Chooser labels and items must have matching rows");

    if (columns <= 0)
    {
      columns = libraryChooserPaneColumns(textCatalog, labels, keymapPlan, 0);
    }

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(labels.size());
    rowHitRegions.clear();
    rowHitRegions.reserve(labels.size());
    std::int32_t focusRow = 0;

    for (std::size_t index = 0; index < labels.size(); ++index)
    {
      if (!search.matches(labels[index]))
      {
        continue;
      }

      auto const selectedRow = std::cmp_equal(index, selected);

      if (selectedRow)
      {
        focusRow = static_cast<std::int32_t>(rows.size());
      }

      rowHitRegions.push_back({.id = items[index].id});
      rows.push_back(
        {.elementPtr = text(labels[index]) | flex, .selected = selectedRow, .box = &rowHitRegions.back().box});
    }

    return style::titledPanel(
             overlayLabel(textCatalog, Overlay::ListChooser),
             vbox({
               style::panelBody(search.render(textCatalog)),
               (labels.empty() ? style::panelBody : style::scrollablePanelBody)(
                 selectableList(std::move(rows),
                                SelectableListOptions{.focusRow = focusRow,
                                                      .emptyText = std::string{i18n::requiredText(
                                                        textCatalog,
                                                        search.isActive() ? i18n::MessageId::TuiListSearchEmpty
                                                                          : i18n::MessageId::TuiLibraryNoListsFound)},
                                                      .framed = !labels.empty(),
                                                      .horizontalScroll = false,
                                                      .scrollIndicator = !labels.empty(),
                                                      .flex = !labels.empty(),
                                                      .centerEmpty = labels.empty(),
                                                      .viewportBox = &viewportBox})),
               style::panelBody(separator()),
               style::panelBody(style::panelFooterHint(
                 search.isActive() ? std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiListSearchHint)}
                                   : overlayHint(textCatalog, keymapPlan, Overlay::ListChooser))),
             })) |
           size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
