// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "NavigationPanel.h"

#include "Keymap.h"
#include "ListNavigationModel.h"
#include "MouseBindings.h"
#include "SelectableList.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  NavigationGeometry navigationGeometry(std::int32_t const terminalColumns,
                                        std::int32_t const detailColumns,
                                        bool const enabled,
                                        bool const focused,
                                        bool const suspended)
  {
    constexpr std::int32_t kMinimumTrackColumns = 72;
    auto const canDock = terminalColumns >= detailColumns + kNavigationColumns + kMinimumTrackColumns + 2;
    auto const docked = enabled && canDock;
    return {.canDock = canDock,
            .docked = docked,
            .drawer = enabled && !canDock && focused && !suspended,
            .columns = std::clamp(terminalColumns, 1, kNavigationColumns),
            .trackColumns = std::max(1, terminalColumns - detailColumns - (docked ? kNavigationColumns : 0) - 2)};
  }

  ftxui::Element navigationPanel(i18n::MessageCatalog const& catalog,
                                 ListNavigationModel const& model,
                                 ListId const activeList,
                                 KeymapPlan const& keymap,
                                 NavigationPanelOptions const options)
  {
    using namespace ftxui;
    auto* const regions = options.regions;
    constexpr std::int32_t kNavigationMarkerColumns = 5;
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
      auto disclosurePtr = text(row.hasChildren ? disclosure : "  ");

      if (hit != nullptr && row.hasChildren)
      {
        disclosurePtr = std::move(disclosurePtr) | reflect(hit->disclosure);
      }

      auto labelPtr = hbox(
        {text(std::string(static_cast<std::size_t>(indent), ' ')),
         std::move(disclosurePtr),
         text(row.id == activeList ? "* " : "  "),
         text(ellipsizeToCellWidth(row.name, std::max(0, bodyColumns - indent - kNavigationMarkerColumns))) | flex});

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

    auto const unavailable = std::to_array({Event::ArrowLeft,
                                            Event::ArrowRight,
                                            Event::Return,
                                            Event::Character("j"),
                                            Event::Character("k"),
                                            Event::Character("/")});
    auto hint = std::string{};

    if (options.focused)
    {
      auto const focusKey = keymap.shortcutFor(KeyAction::SwitchWorkspaceFocus, unavailable);
      auto const returnKey = model.search().isActive() ? std::string_view{"Tab"} : focusKey;
      hint = i18n::requiredFormat(
        catalog, i18n::MessageId::TuiNavigationReturn, {{"key", returnKey.empty() ? "Esc" : returnKey}});
    }
    else if (auto const focusKey = keymap.shortcutFor(KeyAction::SwitchWorkspaceFocus); !focusKey.empty())
    {
      hint =
        std::string{focusKey} + " " + std::string{i18n::requiredText(catalog, i18n::MessageId::TuiShellStatusLists)};
    }

    auto const selectedIt = std::ranges::find(model.rows(), model.cursor(), &ListNavigationRow::id);
    auto const detail = selectedIt == model.rows().end() ? std::string{} : selectedIt->detail;

    auto bodyPtr = vbox({model.search().render(catalog, options.focused, regions != nullptr),
                         selectableList(std::move(rows),
                                        {.focusRow = model.selectedIndex(),
                                         .emptyText = i18n::requiredText(catalog, i18n::MessageId::TuiListSearchEmpty),
                                         .horizontalScroll = false,
                                         .flex = true,
                                         .viewportBox = regions == nullptr ? nullptr : &regions->panel.navigationBox}),
                         text(ellipsizeToCellWidth(detail, bodyColumns)) | dim,
                         separator(),
                         text(ellipsizeToCellWidth(hint, bodyColumns)) | dim});
    auto panelPtr =
      style::popupPanel(i18n::requiredText(catalog, i18n::MessageId::TuiShellOverlayLists), std::move(bodyPtr)) |
      size(WIDTH, EQUAL, options.columns);
    return regions == nullptr ? std::move(panelPtr) : mousePanel(std::move(panelPtr), regions->panel);
  }
} // namespace ao::tui
