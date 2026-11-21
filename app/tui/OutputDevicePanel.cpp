// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "OutputDevicePanel.h"

#include "AnchoredOverlay.h"
#include "SelectableList.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

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
    using RowKind = uimodel::OutputDeviceRow::Kind;

    constexpr std::int32_t kOutputDeviceRows = 14;
    constexpr std::size_t kMaximumStatusLines = 3;
    // The shared frame draws "─ " before the title, " ─ " between the labels, and " " after them.
    constexpr std::int32_t kTitlePaddingColumns = 3;
    constexpr std::int32_t kTitleSeparatorColumns = 3;

    bool isOutputDeviceListRow(uimodel::OutputDeviceRow const& row)
    {
      return row.kind != RowKind::Status; // The footer already shows this advisory.
    }

    std::int32_t outputDeviceHeaderColumns(std::string_view const title, std::string_view const summary)
    {
      return cellWidth(title) + kTitlePaddingColumns + kTitleSeparatorColumns + cellWidth(summary);
    }

    std::string outputDeviceSummary(uimodel::OutputDeviceViewState const* const outputView)
    {
      if (outputView == nullptr || outputView->outputBackendSummary.empty())
      {
        return "--";
      }

      return outputView->outputBackendSummary;
    }

    std::string outputDeviceFooter(i18n::MessageCatalog const& textCatalog, uimodel::OutputDeviceViewState const& view)
    {
      if (!view.outputDeviceStatus.empty())
      {
        return view.outputDeviceStatus;
      }

      return std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiPlaybackNoOutputDeviceSelected)};
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

    ftxui::Elements wrappedOutputLines(std::string_view const value,
                                       std::string_view const prefix,
                                       std::int32_t const columns,
                                       bool const dimmed,
                                       std::size_t const maxLines = 0)
    {
      auto const prefixColumns = cellWidth(prefix);
      auto const indent = std::string(static_cast<std::size_t>(prefixColumns), ' ');
      auto lines = ftxui::Elements{};

      for (auto const& line : wrapCellText(value,
                                           std::max(1, columns - prefixColumns),
                                           {.lineBreaks = CellLineBreaks::Flatten, .maxLines = maxLines}))
      {
        lines.push_back(
          outputText(std::string{lines.empty() ? prefix : std::string_view{indent}} + line, columns, dimmed));
      }

      return lines;
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

  std::int32_t outputDevicePanelColumns(i18n::MessageCatalog const& textCatalog,
                                        uimodel::OutputDeviceViewState const& view,
                                        KeymapPlan const& keymapPlan,
                                        std::int32_t const terminalColumns)
  {
    auto const title = i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOutputDevicesTitle);
    auto contentColumns =
      std::max({outputDeviceHeaderColumns(title, outputDeviceSummary(&view)) - (2 * style::kPanelBodyPaddingColumns),
                cellWidth(outputDeviceFooter(textCatalog, view)),
                cellWidth(overlayHint(textCatalog, keymapPlan, Overlay::OutputDevices))});

    if (std::ranges::none_of(view.rows, isOutputDeviceListRow))
    {
      contentColumns = std::max(
        contentColumns, cellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TuiPlaybackNoOutputDevicesFound)));
    }

    for (auto const& row : view.rows)
    {
      if (!isOutputDeviceListRow(row))
      {
        continue;
      }

      if (row.kind == RowKind::BackendHeader)
      {
        contentColumns = std::max(contentColumns, cellWidth(row.title));
        continue;
      }

      auto const isRoutingPolicy = row.kind == RowKind::RoutingPolicy;
      contentColumns = std::max(contentColumns, cellWidth(row.title) + cellWidth(isRoutingPolicy ? "[ ] " : "* "));

      // The checkbox explanation wraps instead of widening the whole picker.
      if (!isRoutingPolicy && !row.description.empty())
      {
        contentColumns = std::max(contentColumns, cellWidth(row.description) + cellWidth("  "));
      }
    }

    auto const availableColumns = terminalColumns > 0 ? std::max(1, terminalColumns - kAnchoredOverlayHaloColumns) : 0;
    return style::popupPanelColumnsForContent(contentColumns, availableColumns);
  }

  ftxui::Element outputDevicePanel(i18n::MessageCatalog const& textCatalog,
                                   uimodel::OutputDeviceViewState const& view,
                                   std::int32_t const selectedRow,
                                   KeymapPlan const& keymapPlan,
                                   std::vector<OutputDeviceRowHitRegion>* const rowHitRegions,
                                   std::int32_t columns,
                                   ftxui::Box* viewportBox)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = outputDevicePanelColumns(textCatalog, view, keymapPlan, 0);
    }

    // The list scrollbar occupies the same right gutter that panelBody leaves beside the footer.
    auto const bodyColumns = style::popupPanelBodyColumns(columns);
    auto rows = Elements{};
    auto listRows = std::vector<SelectableListRow>{};
    std::int32_t focusRow = 0;
    std::int32_t extraTitleRows = 0;

    if (rowHitRegions != nullptr)
    {
      rowHitRegions->clear();
      rowHitRegions->reserve(view.rows.size());
    }

    listRows.reserve(view.rows.size());

    for (std::size_t index = 0; index < view.rows.size(); ++index)
    {
      auto const& row = view.rows[index];

      if (!isOutputDeviceListRow(row))
      {
        continue;
      }

      if (row.kind == RowKind::BackendHeader)
      {
        listRows.push_back(SelectableListRow{.elementPtr = outputText(row.title, bodyColumns, true)});
        continue;
      }

      auto const isRoutingPolicy = row.kind == RowKind::RoutingPolicy;
      auto const selected = std::cmp_equal(index, selectedRow);
      auto* rowBox = static_cast<ftxui::Box*>(nullptr);
      auto* secondaryBox = static_cast<ftxui::Box*>(nullptr);

      if (selected)
      {
        focusRow = static_cast<std::int32_t>(listRows.size()) + extraTitleRows;
      }

      if (rowHitRegions != nullptr)
      {
        rowHitRegions->push_back(OutputDeviceRowHitRegion{.rowIndex = static_cast<std::int32_t>(index),
                                                          .kind = row.kind,
                                                          .backendId = row.backendId,
                                                          .deviceId = row.deviceId,
                                                          .profileId = row.profileId,
                                                          .optRoutingPolicySelection = row.optRoutingPolicySelection});
        rowBox = &rowHitRegions->back().box;
        secondaryBox = &rowHitRegions->back().secondaryBox;
      }

      if (isRoutingPolicy)
      {
        auto titleLines = wrappedOutputLines(row.title, row.isActive ? "[x] " : "[ ] ", bodyColumns, false);
        extraTitleRows += static_cast<std::int32_t>(titleLines.size()) - 1;
        listRows.push_back(
          SelectableListRow{.elementPtr = vbox(std::move(titleLines)), .selected = selected, .box = rowBox});

        for (auto& linePtr : wrappedOutputLines(row.description, "  ", bodyColumns, true))
        {
          listRows.push_back(SelectableListRow{.elementPtr = std::move(linePtr)});
        }

        continue;
      }

      auto titleLine = std::string{row.isActive ? "* " : "  "} + row.title;
      listRows.push_back(SelectableListRow{
        .elementPtr = outputText(std::move(titleLine), bodyColumns), .selected = selected, .box = rowBox});

      if (!row.description.empty())
      {
        listRows.push_back(
          SelectableListRow{.elementPtr = outputText("  " + row.description, bodyColumns, true), .box = secondaryBox});
      }
    }

    rows.push_back(style::scrollablePanelBody(selectableList(
      std::move(listRows),
      SelectableListOptions{
        .focusRow = focusRow,
        .height = kOutputDeviceRows,
        .emptyText = std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiPlaybackNoOutputDevicesFound)},
        .horizontalScroll = false,
        .viewportBox = viewportBox})));
    rows.push_back(style::panelBody(separator()));
    rows.push_back(style::panelBody(
      vbox(wrappedOutputLines(outputDeviceFooter(textCatalog, view), "", bodyColumns, true, kMaximumStatusLines))));
    rows.push_back(style::panelBody(
      vbox(wrappedOutputLines(overlayHint(textCatalog, keymapPlan, Overlay::OutputDevices), "", bodyColumns, true))));

    auto const title = i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOutputDevicesTitle);
    auto const summary = outputDeviceSummary(&view);
    auto const headerColumns = columns - kPanelBorderColumns;
    // Keep the full title when the two labels compete for space; the footer carries the routing status.
    auto const showSummary = outputDeviceHeaderColumns(title, summary) <= headerColumns;
    return style::titledPanel(ellipsizeToCellWidth(title, headerColumns - kTitlePaddingColumns),
                              vbox(std::move(rows)),
                              style::PanelOptions{.rightTitle = showSummary ? std::string_view{summary} : ""}) |
           size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
