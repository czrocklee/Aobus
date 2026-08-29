// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandPalettePanel.h"

#include "CommandCompletion.h"
#include "SelectableList.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include "TuiText.h"
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
    constexpr std::int32_t kQuickFilterPanelChromeRows = 4;
    constexpr std::int32_t kQuickFilterErrorRows = 2;

    struct CommandPaletteEntryDescriptor final
    {
      std::string_view category{};
      std::string_view shortcut{};
    };

    ftxui::Element fixedText(std::string_view const value,
                             std::int32_t const columns,
                             CellAlignment const alignment = CellAlignment::Left)
    {
      return ftxui::text(fitCellText(value, columns, alignment)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, columns);
    }

    std::string commandPrefixDisplayText(std::string_view prefix)
    {
      if (!prefix.empty() && prefix.back() == ' ')
      {
        prefix.remove_suffix(1);
      }

      return ":" + std::string{prefix};
    }

    std::optional<CommandPaletteEntryDescriptor> commandPaletteEntryDescriptor(i18n::MessageCatalog const& textCatalog,
                                                                               rt::CompletionItem const& item)
    {
      for (auto const& spec : commandPrefixSpecs())
      {
        if (item.insertText == spec.prefix && item.displayText == commandPrefixDisplayText(spec.prefix))
        {
          return CommandPaletteEntryDescriptor{
            .category = i18n::requiredText(textCatalog, spec.category),
            .shortcut = shortcutFor(spec.optShortcutAction.value_or(spec.action)),
          };
        }
      }

      for (auto const& spec : commandAliasSpecs())
      {
        if (item.insertText == spec.alias && item.displayText == ":" + std::string{spec.alias})
        {
          return CommandPaletteEntryDescriptor{
            .category = i18n::requiredText(textCatalog, spec.category), .shortcut = shortcutFor(spec.action)};
        }
      }

      return std::nullopt;
    }

    std::string commandPaletteTrailingText(i18n::MessageCatalog const& textCatalog, rt::CompletionItem const& item)
    {
      if (auto const optDescriptor = commandPaletteEntryDescriptor(textCatalog, item);
          optDescriptor && !optDescriptor->shortcut.empty())
      {
        return std::string{optDescriptor->shortcut};
      }

      return uimodel::completionDetail(textCatalog, item.detail);
    }

    std::vector<SelectableListRow> commandCompletionRows(rt::CompletionResult const& completion,
                                                         i18n::MessageCatalog const& textCatalog,
                                                         std::int32_t const selectedIndex,
                                                         std::int32_t const contentColumns)
    {
      using namespace ftxui;

      std::int32_t categoryColumns = 0;
      std::int32_t trailingColumns = 0;

      for (auto const& item : completion.items)
      {
        if (auto const optDescriptor = commandPaletteEntryDescriptor(textCatalog, item); optDescriptor)
        {
          categoryColumns = std::max(categoryColumns, cellWidth(optDescriptor->category));
        }

        trailingColumns = std::max(trailingColumns, cellWidth(commandPaletteTrailingText(textCatalog, item)));
      }

      categoryColumns = std::min(categoryColumns, contentColumns);
      trailingColumns = std::min(trailingColumns, contentColumns);

      auto rows = std::vector<SelectableListRow>{};
      rows.reserve(completion.items.size());

      for (std::size_t index = 0; index < completion.items.size(); ++index)
      {
        auto const& item = completion.items[index];
        auto cells = Elements{};
        cells.reserve(kCommandCompletionRowCellReserve);
        auto const selected = std::cmp_equal(index, selectedIndex);
        cells.push_back(fixedText(selected ? "> " : "  ", 2));

        if (categoryColumns > 0)
        {
          auto categoryPtr = fixedText(
            commandPaletteEntryDescriptor(textCatalog, item).value_or(CommandPaletteEntryDescriptor{}).category,
            categoryColumns);
          cells.push_back(selected ? std::move(categoryPtr) : std::move(categoryPtr) | style::accent() | dim);
          cells.push_back(text("  "));
        }

        cells.push_back(text(item.displayText) | flex);

        if (trailingColumns > 0)
        {
          auto trailingPtr =
            fixedText(commandPaletteTrailingText(textCatalog, item), trailingColumns, CellAlignment::Right);
          cells.push_back(text("  "));
          cells.push_back(selected ? std::move(trailingPtr) : std::move(trailingPtr) | style::accent());
        }

        auto rowPtr = hbox(std::move(cells));

        rows.push_back(SelectableListRow{.elementPtr = std::move(rowPtr), .selected = selected});
      }

      return rows;
    }

    ftxui::Element commandCompletionList(i18n::MessageCatalog const& textCatalog,
                                         ShellInteractionModel const& shell,
                                         std::int32_t const contentColumns)
    {
      if (auto const& optCompletion = shell.commandCompletion(); optCompletion && !optCompletion->items.empty())
      {
        return selectableList(
          commandCompletionRows(*optCompletion, textCatalog, shell.commandCompletionSelection(), contentColumns),
          SelectableListOptions{.focusRow = shell.commandCompletionSelection(), .flex = true});
      }

      return selectableList(
        {},
        SelectableListOptions{.emptyText = tuiChromeText(textCatalog, i18n::MessageId::TuiShellCommandPaletteNoMatches),
                              .flex = true,
                              .centerEmpty = true});
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
                                  : 1;
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
                                     std::int32_t columns)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = commandPalettePanelColumns(0);
    }

    auto const suffix = commandCompletionSuffix(shell);
    auto rows = Elements{};
    rows.push_back(hbox({
      text("> ") | style::accent() | bold,
      text(":") | style::accent() | bold,
      text(shell.inputDraft()) | bold,
      text(suffix) | dim,
      text("_") | style::accent() | bold,
    }));
    rows.push_back(separator());

    auto const contentColumns = style::popupPanelBodyColumns(columns);
    rows.push_back(commandCompletionList(textCatalog, shell, contentColumns));

    rows.push_back(separator());
    rows.push_back(style::panelFooterHint(tuiChromeText(textCatalog, i18n::MessageId::TuiShellCommandPaletteFooter)));

    return style::popupPanel(
             tuiChromeText(textCatalog, i18n::MessageId::TuiShellCommandPaletteTitle), vbox(std::move(rows))) |
           size(WIDTH, EQUAL, columns);
  }

  ftxui::Element quickFilterCompletionPanel(i18n::MessageCatalog const& textCatalog,
                                            ShellInteractionModel const& shell,
                                            std::int32_t columns,
                                            std::string_view const filterError)
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
      rows.push_back(fixedText(filterError, contentColumns) | style::danger());
      rows.push_back(separator());
    }

    rows.push_back(commandCompletionList(textCatalog, shell, contentColumns));
    rows.push_back(separator());
    rows.push_back(style::panelFooterHint(tuiChromeText(textCatalog, i18n::MessageId::TuiShellQuickFilterFooter)));

    return style::popupPanel(
             tuiChromeText(textCatalog, i18n::MessageId::TuiShellQuickFilterTitle), vbox(std::move(rows))) |
           size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
