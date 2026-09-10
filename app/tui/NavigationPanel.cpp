// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "NavigationPanel.h"

#include "ListNavigationModel.h"
#include "PanelWidths.h"
#include "SelectableList.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/CoreIds.h>
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
    ftxui::Element navigationExpression(std::string_view const value, std::int32_t const columns)
    {
      using namespace ftxui;
      auto rows = Elements{};

      for (auto& line : wrapCellText(value, columns, {.lineBreaks = CellLineBreaks::Flatten, .maxLines = 2}))
      {
        rows.push_back(text(std::move(line)));
      }

      return vbox(std::move(rows)) | dim;
    }
  } // namespace

  NavigationGeometry navigationGeometry(std::int32_t const terminalColumns,
                                        std::int32_t const detailColumns,
                                        bool const pinned,
                                        bool const separateBorders,
                                        PanelWidths const widths)
  {
    constexpr auto kTrackPaddingColumns = style::kPanelBodyPaddingColumns;
    auto const overlap = separateBorders ? 0 : 1;
    auto const trackFrame = 2 + kTrackPaddingColumns;
    auto const minimumNavigation = widths.navigation > 0 ? kMinimumNavigationColumns : kNavigationColumns;
    auto const navigationReserve = pinned ? minimumNavigation - overlap : 0;
    auto const detailLimit =
      std::max(1, terminalColumns - kMinimumTrackColumns - trackFrame - navigationReserve + overlap);
    auto const requestedDetail = widths.detail > 0 ? std::max(kMinimumDetailColumns, widths.detail) : detailColumns;
    auto actualDetail = detailColumns;

    if (detailColumns > 0 && widths.detail > 0)
    {
      auto const minimumDetail = std::min(kMinimumDetailColumns, std::max(1, terminalColumns / 2));
      actualDetail = std::min(requestedDetail, std::max(minimumDetail, detailLimit));
    }

    auto const detailWidth = std::max(0, actualDetail - overlap);
    auto const availableNavigation = terminalColumns - detailWidth - kMinimumTrackColumns - trackFrame + overlap;
    auto const canDock = availableNavigation >= minimumNavigation;
    auto const docked = pinned && canDock;
    auto const requestedNavigation =
      widths.navigation > 0 ? std::max(kMinimumNavigationColumns, widths.navigation) : kNavigationColumns;
    auto const columns = std::max(1, std::min(requestedNavigation, canDock ? availableNavigation : terminalColumns));
    return {.canDock = canDock,
            .docked = docked,
            .columns = columns,
            .trackColumns = std::max(1, terminalColumns - detailWidth - (docked ? columns - overlap : 0) - trackFrame),
            .detailColumns = actualDetail,
            .terminalColumns = terminalColumns};
  }

  ftxui::Element dockNavigationPanel(ftxui::Element navigationPtr,
                                     ftxui::Element workspacePtr,
                                     std::int32_t const navigationColumns,
                                     ftxui::Box* const pinBox,
                                     bool const hovered,
                                     style::PanelDividerOptions const options)
  {
    using namespace ftxui;
    auto const dividerColumn = std::max(0, navigationColumns - 1);
    auto offset = [](std::int32_t columns) { return filler() | size(WIDTH, EQUAL, columns); };
    auto const workspaceColumn = dividerColumn + (options.separateBorders ? 1 : 0);

    return dbox({hbox({std::move(navigationPtr), filler()}),
                 hbox({offset(workspaceColumn), std::move(workspacePtr) | flex}),
                 hbox({offset(dividerColumn), style::panelDivider("‹", pinBox, hovered, options), filler()})});
  }

  ftxui::Element collapsedNavigationPanel(ftxui::Element workspacePtr,
                                          ftxui::Box& pinBox,
                                          bool const hovered,
                                          bool const revealOnHover,
                                          ftxui::Box* const hoverBox)
  {
    using namespace ftxui;
    auto edgePtr = vbox({filler(), style::panelIndicator("›", pinBox, hovered, revealOnHover), filler()});

    if (hoverBox != nullptr)
    {
      edgePtr = std::move(edgePtr) | reflect(*hoverBox);
    }

    return dbox({std::move(workspacePtr), hbox({std::move(edgePtr), filler()})});
  }

  ftxui::Element navigationPanel(i18n::MessageCatalog const& catalog,
                                 ListNavigationModel const& model,
                                 ListId const activeList,
                                 NavigationPanelOptions const options)
  {
    using namespace ftxui;
    auto* const regions = options.regions;
    constexpr std::int32_t kNavigationMarkerColumns = 2;
    auto const showDisclosure = std::ranges::any_of(model.rows(), &ListNavigationRow::hasChildren);
    auto const disclosureColumns = showDisclosure ? 2 : 0;
    auto const bodyColumns = style::popupPanelBodyColumns(options.columns);
    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(model.rows().size());

    if (regions != nullptr)
    {
      *regions = {};
      regions->revision = model.revision();
      regions->rows.reserve(model.rows().size());
    }

    for (auto const& row : model.rows())
    {
      auto* hit = static_cast<NavigationRowHit*>(nullptr);

      if (regions != nullptr)
      {
        regions->rows.push_back({.id = row.id});
        hit = &regions->rows.back();
      }

      auto const indent =
        static_cast<std::int32_t>(std::min(row.depth, static_cast<std::size_t>(std::max(0, bodyColumns / 6)))) * 2;
      auto const* disclosure = row.expanded ? "▾ " : "▸ ";
      auto const* emptyDisclosure = showDisclosure ? "  " : "";
      auto disclosurePtr = text(row.hasChildren ? disclosure : emptyDisclosure);

      if (hit != nullptr && row.hasChildren)
      {
        disclosurePtr = std::move(disclosurePtr) | reflect(hit->disclosure);
      }

      auto labelPtr =
        hbox({text(std::string(static_cast<std::size_t>(indent), ' ')),
              std::move(disclosurePtr),
              text(row.id == activeList ? "* " : "  "),
              text(ellipsizeToCellWidth(
                row.name, std::max(0, bodyColumns - indent - kNavigationMarkerColumns - disclosureColumns))) |
                flex});

      if (row.id == activeList)
      {
        labelPtr = std::move(labelPtr) | bold | style::accent();
      }

      if (!row.matching)
      {
        labelPtr = std::move(labelPtr) | dim;
      }

      rows.push_back({.elementPtr = std::move(labelPtr),
                      .selected = options.focused && row.id == model.cursor(),
                      .box = hit == nullptr ? nullptr : &hit->row});
    }

    auto const selectedIt = std::ranges::find(model.rows(), model.cursor(), &ListNavigationRow::id);
    auto const detail = selectedIt == model.rows().end() ? std::string{} : selectedIt->detail;

    auto listPtr = style::scrollablePanelBody(
      selectableList(std::move(rows),
                     {.focusRow = model.selectedIndex(),
                      .emptyText = i18n::requiredText(catalog, i18n::MessageId::TuiListSearchEmpty),
                      .horizontalScroll = false,
                      .flex = true,
                      .viewportBox = regions == nullptr ? nullptr : &regions->panel.navigationBox}));
    auto content = Elements{model.search().isActive()
                              ? style::panelBody(model.search().render(catalog, options.focused, regions != nullptr))
                              : emptyElement(),
                            std::move(listPtr) | flex};

    if (!detail.empty())
    {
      content.push_back(style::panelBody(navigationExpression(detail, bodyColumns)));
    }

    auto bodyPtr = vbox(std::move(content));
    auto panelPtr = style::titledPanel("", std::move(bodyPtr)) | size(WIDTH, EQUAL, options.columns);

    if (regions == nullptr)
    {
      return panelPtr;
    }

    return std::move(panelPtr) | reflect(regions->panel.box);
  }
} // namespace ao::tui
