// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "NotificationCenterPanel.h"

#include "StatusBar.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::string_view kNotificationCenterPanelFooter =
      "n toggle  x hide compact  click clearable row  Esc close";
    constexpr std::int32_t kActivityProgressRailColumns = 10;
  } // namespace

  std::int32_t notificationCenterPanelColumns(uimodel::ActivityStatusViewState const& state,
                                              std::int32_t const terminalColumns)
  {
    auto contentColumns = std::max(cellWidth("Notifications"), cellWidth(kNotificationCenterPanelFooter));

    if (state.compact.kind != uimodel::ActivityStatusKind::Idle)
    {
      contentColumns = std::max(contentColumns,
                                cellWidth(activityKindLabel(state.compact.kind)) + cellWidth(" ") +
                                  cellWidth(state.compact.text) + cellWidth(" …"));
    }

    if (state.detail.optLibraryTask)
    {
      contentColumns = std::max(contentColumns, cellWidth("Library task"));
      contentColumns = std::max(contentColumns, cellWidth(state.detail.optLibraryTask->message));
    }

    for (auto const& item : state.detail.items)
    {
      auto const title = item.title.empty() ? item.message : item.title;
      contentColumns = std::max(contentColumns, cellWidth(title) + cellWidth("error "));

      if (!item.title.empty() && !item.message.empty())
      {
        contentColumns = std::max(contentColumns, cellWidth(item.message) + cellWidth("  "));
      }

      if (item.dismissible)
      {
        contentColumns = std::max(contentColumns, cellWidth(title) + cellWidth("  x"));
      }
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element notificationCenterPanel(uimodel::ActivityStatusViewState const& state,
                                         std::vector<NotificationDetailRowHitRegion>* const rowHitRegions,
                                         std::int32_t const columns)
  {
    using namespace ftxui;

    auto const panelColumns = columns <= 0 ? notificationCenterPanelColumns(state, 0) : columns;

    if (rowHitRegions != nullptr)
    {
      rowHitRegions->clear();
      rowHitRegions->reserve(state.detail.items.size());
    }

    auto rows = Elements{};

    if (state.compact.kind != uimodel::ActivityStatusKind::Idle && !state.compact.text.empty())
    {
      rows.push_back(activityCompactLine(state.compact));
      rows.push_back(separator());
    }

    if (state.detail.optLibraryTask)
    {
      rows.push_back(text("Library task") | style::accent() | bold);
      rows.push_back(hbox({
        text(state.detail.optLibraryTask->message) | flex,
        text(" "),
        text(activityProgressRail(state.detail.optLibraryTask->progressFraction, kActivityProgressRailColumns)) |
          style::accent(),
      }));
      rows.push_back(separator());
    }

    for (auto const& item : state.detail.items)
    {
      auto const itemKind = activityKindForSeverity(item.severity);
      auto const title = item.title.empty() ? item.message : item.title;
      auto bodyRows = Elements{};
      bodyRows.push_back(text(title));

      if (!item.title.empty() && !item.message.empty())
      {
        bodyRows.push_back(text(item.message) | dim);
      }

      if (item.optProgressMode)
      {
        auto progressText = *item.optProgressMode == rt::NotificationProgressMode::Fraction
                              ? activityProgressRail(item.progressFraction, kActivityProgressRailColumns)
                              : std::string{"[working]"};

        if (!item.progressLabel.empty())
        {
          progressText.append(" ");
          progressText.append(item.progressLabel);
        }

        bodyRows.push_back(text(std::move(progressText)) | style::accent());
      }

      auto rowPtr = hbox({
        text(std::string{activityKindLabel(itemKind)}) | activityKindColor(itemKind) | bold,
        text(" "),
        vbox(std::move(bodyRows)) | flex,
        item.dismissible ? text(" x") | dim : emptyElement(),
      });

      if (rowHitRegions != nullptr)
      {
        rowHitRegions->push_back(NotificationDetailRowHitRegion{.id = item.id, .dismissible = item.dismissible});
        rowPtr = std::move(rowPtr) | reflect(rowHitRegions->back().box);
      }

      rows.push_back(std::move(rowPtr));
    }

    if (rows.empty())
    {
      rows.push_back(text("No notifications") | dim);
    }

    rows.push_back(separator());
    rows.push_back(style::panelFooterHint(kNotificationCenterPanelFooter));

    return style::popupPanel("Notifications", vbox(std::move(rows))) | size(WIDTH, EQUAL, panelColumns);
  }
} // namespace ao::tui
