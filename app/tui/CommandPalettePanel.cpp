// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandPalettePanel.h"

#include "Command.h"
#include "Keymap.h"
#include "MouseBindings.h"
#include "SelectableList.h"
#include "ShellInteractionModel.h"
#include "ShellText.h"
#include "Style.h"
#include "TextCell.h"
#include "TextField.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kCommandPaletteDefaultColumns = 72;
    constexpr std::int32_t kCommandPaletteMinColumns = 56;
    constexpr std::int32_t kCommandPaletteDefaultRows = 18;
    constexpr std::int32_t kCommandPaletteMinRows = 12;
    constexpr std::int32_t kCommandPaletteMaxRows = 20;
    constexpr double kCommandPaletteWidthRatio = 0.40;
    constexpr double kCommandPaletteHeightRatio = 0.35;
    constexpr std::size_t kCommandCompletionRowCellReserve = 6;
    constexpr std::int32_t kQuickFilterPanelChromeRows = 5;
    constexpr std::int32_t kQuickFilterErrorRows = 2;

    struct CommandPaletteEntryDescriptor final
    {
      std::string_view category{};
      std::string shortcut{};
    };

    ftxui::Element fixedText(std::string_view const value,
                             std::int32_t const columns,
                             CellAlignment const alignment = CellAlignment::Left)
    {
      return ftxui::text(fitCellText(value, columns, alignment)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, columns);
    }

    std::optional<CommandPaletteEntryDescriptor> commandPaletteEntryDescriptor(i18n::MessageCatalog const& textCatalog,
                                                                               rt::CompletionItem const& item,
                                                                               KeymapPlan const& keymapPlan)
    {
      for (auto const& spec : commandPrefixSpecs())
      {
        if (item.insertText == spec.prefix && item.displayText == chromeText(textCatalog, spec.detail))
        {
          return CommandPaletteEntryDescriptor{
            .category = i18n::requiredText(textCatalog, spec.category),
            .shortcut =
              spec.optShortcutAction ? std::string{keymapPlan.shortcutFor(*spec.optShortcutAction)} : std::string{},
          };
        }
      }

      for (auto const& spec : commandAliasSpecs())
      {
        if (item.insertText == spec.alias && item.displayText == chromeText(textCatalog, spec.detail))
        {
          return CommandPaletteEntryDescriptor{.category = i18n::requiredText(textCatalog, spec.category),
                                               .shortcut = commandShortcut(keymapPlan, spec.action)};
        }
      }

      return std::nullopt;
    }

    std::string commandPaletteTrailingText(i18n::MessageCatalog const& textCatalog,
                                           rt::CompletionItem const& item,
                                           KeymapPlan const& keymapPlan)
    {
      if (auto const optDescriptor = commandPaletteEntryDescriptor(textCatalog, item, keymapPlan);
          optDescriptor && !optDescriptor->shortcut.empty())
      {
        return std::string{optDescriptor->shortcut};
      }

      return uimodel::completionDetail(textCatalog, item.detail);
    }

    std::vector<SelectableListRow> commandCompletionRows(rt::CompletionResult const& completion,
                                                         i18n::MessageCatalog const& textCatalog,
                                                         KeymapPlan const& keymapPlan,
                                                         std::int32_t const selectedIndex,
                                                         std::int32_t const contentColumns,
                                                         CompletionHitRegions* hitRegions)
    {
      using namespace ftxui;

      std::int32_t categoryColumns = 0;
      std::int32_t trailingColumns = 0;

      for (auto const& item : completion.items)
      {
        if (auto const optDescriptor = commandPaletteEntryDescriptor(textCatalog, item, keymapPlan); optDescriptor)
        {
          categoryColumns = std::max(categoryColumns, cellWidth(optDescriptor->category));
        }

        trailingColumns =
          std::max(trailingColumns, cellWidth(commandPaletteTrailingText(textCatalog, item, keymapPlan)));
      }

      // Keep the completion label readable when category or detail text is long.
      categoryColumns = std::min(categoryColumns, std::max(0, (contentColumns / 4) - 2));
      trailingColumns = std::min(trailingColumns, std::max(0, (contentColumns / 3) - 2));
      auto const labelColumns = std::max(0,
                                         contentColumns - 2 - (categoryColumns > 0 ? categoryColumns + 2 : 0) -
                                           (trailingColumns > 0 ? trailingColumns + 2 : 0));

      auto rows = std::vector<SelectableListRow>{};
      rows.reserve(completion.items.size());

      if (hitRegions != nullptr)
      {
        hitRegions->rows.assign(completion.items.size(), kEmptyMouseBox);
        hitRegions->insertions.clear();
        hitRegions->replaceBegin = completion.replaceBegin;
        hitRegions->replaceEnd = completion.replaceEnd;

        for (auto const& item : completion.items)
        {
          hitRegions->insertions.push_back(item.insertText);
        }
      }

      for (std::size_t index = 0; index < completion.items.size(); ++index)
      {
        auto const& item = completion.items[index];
        auto cells = Elements{};
        cells.reserve(kCommandCompletionRowCellReserve);
        auto const selected = std::cmp_equal(index, selectedIndex);
        cells.push_back(fixedText(selected ? "> " : "  ", 2));

        if (categoryColumns > 0)
        {
          auto categoryPtr = fixedText(commandPaletteEntryDescriptor(textCatalog, item, keymapPlan)
                                         .value_or(CommandPaletteEntryDescriptor{})
                                         .category,
                                       categoryColumns);
          cells.push_back(selected ? std::move(categoryPtr) : std::move(categoryPtr) | style::accent() | dim);
          cells.push_back(text("  "));
        }

        cells.push_back(fixedText(item.displayText, labelColumns) | flex);

        if (trailingColumns > 0)
        {
          auto trailingPtr =
            fixedText(commandPaletteTrailingText(textCatalog, item, keymapPlan), trailingColumns, CellAlignment::Right);
          cells.push_back(text("  "));
          cells.push_back(selected ? std::move(trailingPtr) : std::move(trailingPtr) | style::accent());
        }

        auto rowPtr = hbox(std::move(cells));

        rows.push_back(SelectableListRow{.elementPtr = std::move(rowPtr),
                                         .selected = selected,
                                         .box = hitRegions != nullptr ? &hitRegions->rows[index] : nullptr});
      }

      return rows;
    }

    ftxui::Element commandCompletionList(i18n::MessageCatalog const& textCatalog,
                                         ShellInteractionModel const& shell,
                                         KeymapPlan const& keymapPlan,
                                         std::int32_t const contentColumns,
                                         CompletionHitRegions* hitRegions)
    {
      if (hitRegions != nullptr)
      {
        *hitRegions = CompletionHitRegions{.draft = shell.inputDraft(), .cursor = shell.inputField().cursor()};
      }

      if (auto const& optCompletion = shell.commandCompletion(); optCompletion && !optCompletion->items.empty())
      {
        return style::scrollablePanelBody(selectableList(
          commandCompletionRows(
            *optCompletion, textCatalog, keymapPlan, shell.commandCompletionSelection(), contentColumns, hitRegions),
          SelectableListOptions{.focusRow = shell.commandCompletionSelection(),
                                .horizontalScroll = false,
                                .flex = true,
                                .viewportBox = (hitRegions != nullptr) ? &hitRegions->listBox : nullptr}));
      }

      if (shell.inputMode() == ShellInputMode::QuickFilter)
      {
        return style::panelBody(
          ftxui::paragraph(chromeText(textCatalog, i18n::MessageId::TuiQuickFilterNoSuggestions)) | ftxui::dim |
          ftxui::flex);
      }

      return style::scrollablePanelBody(selectableList(
        {},
        SelectableListOptions{.emptyText = chromeText(textCatalog, i18n::MessageId::TuiShellCommandPaletteNoMatches),
                              .horizontalScroll = false,
                              .flex = true,
                              .centerEmpty = true,
                              .viewportBox = (hitRegions != nullptr) ? &hitRegions->listBox : nullptr}));
    }
  } // namespace

  std::int32_t commandPalettePanelColumns(std::int32_t const terminalColumns)
  {
    if (terminalColumns <= 0)
    {
      return kCommandPaletteDefaultColumns;
    }

    auto const proportionalColumns =
      static_cast<std::int32_t>(std::lround(static_cast<double>(terminalColumns) * kCommandPaletteWidthRatio));
    auto const desiredColumns = std::max(kCommandPaletteMinColumns, proportionalColumns);
    return std::min(desiredColumns, terminalColumns);
  }

  std::int32_t commandPalettePanelRows(std::int32_t const terminalRows)
  {
    if (terminalRows <= 0)
    {
      return kCommandPaletteDefaultRows;
    }

    auto const proportionalRows =
      static_cast<std::int32_t>(std::lround(static_cast<double>(terminalRows) * kCommandPaletteHeightRatio));
    auto const desiredRows = std::clamp(proportionalRows, kCommandPaletteMinRows, kCommandPaletteMaxRows);
    return std::min(desiredRows, terminalRows);
  }

  std::int32_t quickFilterPanelRows(ShellInteractionModel const& shell,
                                    bool const hasFilterError,
                                    std::int32_t const terminalRows)
  {
    auto const completionRows = shell.commandCompletion() && !shell.commandCompletion()->items.empty()
                                  ? static_cast<std::int32_t>(shell.commandCompletion()->items.size())
                                  : 2;
    auto const desiredRows =
      completionRows + kQuickFilterPanelChromeRows + (hasFilterError ? kQuickFilterErrorRows : 0);

    if (terminalRows <= 0)
    {
      return desiredRows;
    }

    return std::min(desiredRows, std::max(1, terminalRows - 1));
  }

  ftxui::Element commandPalettePanel(i18n::MessageCatalog const& textCatalog,
                                     ShellInteractionModel const& shell,
                                     KeymapPlan const& keymapPlan,
                                     std::int32_t columns,
                                     CompletionHitRegions* hitRegions)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = commandPalettePanelColumns(0);
    }

    auto rows = Elements{};
    rows.push_back(style::panelBody(hbox({
      text("> ") | style::accent() | bold,
      text(":") | style::accent() | bold,
      textFieldValue(shell.inputField(), hitRegions == nullptr ? nullptr : &hitRegions->inputOrigin) | bold | flex |
        (hitRegions == nullptr ? nothing : reflect(hitRegions->inputBox)),
    })));
    rows.push_back(style::panelBody(separator()));

    auto const contentColumns = style::popupPanelBodyColumns(columns);
    rows.push_back(commandCompletionList(textCatalog, shell, keymapPlan, contentColumns, hitRegions));

    rows.push_back(style::panelBody(separator()));
    rows.push_back(
      style::panelBody(style::panelFooterHint(chromeText(textCatalog, i18n::MessageId::TuiShellCommandPaletteFooter))));
    rows.push_back(
      style::panelBody(style::panelFooterHint(chromeText(textCatalog, i18n::MessageId::TuiInputHistoryHint))));

    return style::titledPanel(
             chromeText(textCatalog, i18n::MessageId::TuiShellCommandPaletteTitle), vbox(std::move(rows))) |
           size(WIDTH, EQUAL, columns);
  }

  ftxui::Element quickFilterCompletionPanel(i18n::MessageCatalog const& textCatalog,
                                            ShellInteractionModel const& shell,
                                            KeymapPlan const& keymapPlan,
                                            std::int32_t columns,
                                            std::string_view const filterError,
                                            CompletionHitRegions* hitRegions)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = commandPalettePanelColumns(0);
    }

    auto rows = Elements{};
    auto const contentColumns = style::popupPanelBodyColumns(columns);

    if (!filterError.empty())
    {
      rows.push_back(style::panelBody(fixedText(filterError, contentColumns) | style::danger()));
      rows.push_back(style::panelBody(separator()));
    }

    rows.push_back(commandCompletionList(textCatalog, shell, keymapPlan, contentColumns, hitRegions));
    rows.push_back(style::panelBody(separator()));
    auto footer = i18n::MessageId::TuiQuickFilterLiteralFooter;

    if (shell.inputDraft().empty())
    {
      footer = i18n::MessageId::TuiQuickFilterEmptyFooter;
    }
    else if (shell.commandCompletion() && !shell.commandCompletion()->items.empty())
    {
      footer = i18n::MessageId::TuiShellQuickFilterFooter;
    }

    rows.push_back(style::panelBody(paragraph(chromeText(textCatalog, footer)) | dim));

    constexpr std::int32_t kHistoryHintColumns = 80;

    if (columns >= kHistoryHintColumns)
    {
      rows.push_back(
        style::panelBody(style::panelFooterHint(chromeText(textCatalog, i18n::MessageId::TuiInputHistoryHint))));
    }

    return style::titledPanel(
             chromeText(textCatalog, i18n::MessageId::TuiShellQuickFilterTitle), vbox(std::move(rows))) |
           size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
