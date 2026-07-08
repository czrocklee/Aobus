// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "OutputDevicePanel.h"

#include "SelectableList.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

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
  namespace
  {
    constexpr std::int32_t kOutputDeviceRows = 14;
    constexpr std::int32_t kPanelScrollIndicatorColumns = 1;

    std::string outputDeviceSummary(uimodel::OutputDeviceViewState const* const outputView)
    {
      if (outputView == nullptr || outputView->outputBackendSummary.empty())
      {
        return "--";
      }

      return outputView->outputBackendSummary;
    }

    std::string outputDeviceFooter(uimodel::OutputDeviceViewState const& view)
    {
      if (!view.outputDeviceStatus.empty())
      {
        return view.outputDeviceStatus;
      }

      return "No output device selected";
    }

    ftxui::Element outputText(std::string value, std::int32_t const columns, bool const dimmed = false)
    {
      auto elementPtr = ftxui::text(fitCellText(value, columns)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, columns);

      if (dimmed)
      {
        elementPtr = elementPtr | ftxui::dim;
      }

      return elementPtr;
    }
  } // namespace

  ftxui::Element outputDeviceBadge(uimodel::OutputDeviceViewState const* const outputView, bool const hovered)
  {
    using namespace ftxui;

    auto elementPtr = text(" " + outputDeviceSummary(outputView) + " ");

    if (hovered)
    {
      return elementPtr | style::buttonHover();
    }

    if (outputView == nullptr || !outputView->hasActiveOutputDevice)
    {
      return elementPtr | dim;
    }

    return elementPtr | style::accent() | bold;
  }

  std::int32_t outputDevicePanelColumns(uimodel::OutputDeviceViewState const& view, std::int32_t const terminalColumns)
  {
    auto contentColumns =
      std::max({cellWidth("Output Devices") + cellWidth(outputDeviceSummary(&view)),
                cellWidth("Output Devices") + cellWidth(" · ") + cellWidth(outputDeviceSummary(&view)),
                cellWidth(outputDeviceFooter(view)),
                cellWidth(overlayHint(Overlay::OutputDevices))});

    if (view.rows.empty())
    {
      contentColumns = std::max(contentColumns, cellWidth("No output devices found") + kPanelScrollIndicatorColumns);
    }

    for (auto const& row : view.rows)
    {
      if (row.kind == uimodel::OutputDeviceRow::Kind::BackendHeader)
      {
        contentColumns = std::max(contentColumns, cellWidth(row.title) + kPanelScrollIndicatorColumns);
        continue;
      }

      contentColumns = std::max(contentColumns, cellWidth(row.title) + cellWidth("* ") + kPanelScrollIndicatorColumns);

      if (!row.description.empty())
      {
        contentColumns =
          std::max(contentColumns, cellWidth(row.description) + cellWidth("  ") + kPanelScrollIndicatorColumns);
      }
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element outputDevicePanel(uimodel::OutputDeviceViewState const& view,
                                   std::int32_t const selectedRow,
                                   std::vector<OutputDeviceRowHitRegion>* const rowHitRegions,
                                   std::int32_t columns)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = outputDevicePanelColumns(view, 0);
    }

    auto const bodyColumns = style::popupPanelBodyColumns(columns);
    auto const listTextColumns = std::max(0, bodyColumns - kPanelScrollIndicatorColumns);
    auto const footerTextColumns = bodyColumns;
    auto rows = Elements{};
    auto listRows = std::vector<SelectableListRow>{};
    std::int32_t focusRow = 0;

    if (rowHitRegions != nullptr)
    {
      rowHitRegions->clear();
      rowHitRegions->reserve(view.rows.size());
    }

    listRows.reserve(view.rows.size());

    for (std::size_t index = 0; index < view.rows.size(); ++index)
    {
      auto const& row = view.rows[index];

      if (row.kind == uimodel::OutputDeviceRow::Kind::BackendHeader)
      {
        listRows.push_back(SelectableListRow{.elementPtr = outputText(row.title, listTextColumns, true)});
        continue;
      }

      auto titleLine = std::string{row.isActive ? "* " : "  "} + row.title;
      auto rowPtr = outputText(std::move(titleLine), listTextColumns);
      auto const selected = std::cmp_equal(index, selectedRow);
      auto* rowBox = static_cast<ftxui::Box*>(nullptr);
      auto* secondaryBox = static_cast<ftxui::Box*>(nullptr);

      if (selected)
      {
        focusRow = static_cast<std::int32_t>(listRows.size());
      }

      if (rowHitRegions != nullptr)
      {
        rowHitRegions->push_back(OutputDeviceRowHitRegion{.rowIndex = static_cast<std::int32_t>(index),
                                                          .backendId = row.backendId,
                                                          .deviceId = row.deviceId,
                                                          .profileId = row.profileId});
        rowBox = &rowHitRegions->back().box;
        secondaryBox = &rowHitRegions->back().secondaryBox;
      }

      listRows.push_back(SelectableListRow{.elementPtr = std::move(rowPtr), .selected = selected, .box = rowBox});

      if (!row.description.empty())
      {
        listRows.push_back(SelectableListRow{
          .elementPtr = outputText("  " + row.description, listTextColumns, true), .box = secondaryBox});
      }
    }

    rows.push_back(
      selectableList(std::move(listRows),
                     SelectableListOptions{
                       .focusRow = focusRow, .height = kOutputDeviceRows, .emptyText = "No output devices found"}));
    rows.push_back(separator());
    rows.push_back(outputText(outputDeviceFooter(view), footerTextColumns, true));
    rows.push_back(outputText(std::string{overlayHint(Overlay::OutputDevices)}, footerTextColumns, true));

    auto const summary = outputDeviceSummary(&view);
    return style::popupPanel("Output Devices", vbox(std::move(rows)), style::PanelOptions{.rightTitle = summary}) |
           size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
