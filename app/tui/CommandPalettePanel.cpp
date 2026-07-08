// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandPalettePanel.h"

#include "SelectableList.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionText.h>

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
    constexpr std::string_view kCommandPaletteFooter = "Tab complete  Enter run  Esc cancel";
    constexpr std::size_t kCommandCompletionRowCellReserve = 6;

    struct CommandPaletteMetadata final
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

      return "/" + std::string{prefix};
    }

    std::optional<CommandPaletteMetadata> commandPaletteMetadata(rt::CompletionItem const& item)
    {
      for (auto const& spec : commandPrefixSpecs())
      {
        if (item.insertText == spec.prefix && item.displayText == commandPrefixDisplayText(spec.prefix))
        {
          return CommandPaletteMetadata{.category = spec.category, .shortcut = spec.shortcut};
        }
      }

      for (auto const& spec : commandAliasSpecs())
      {
        if (item.insertText == spec.alias && item.displayText == "/" + std::string{spec.alias})
        {
          return CommandPaletteMetadata{.category = spec.category, .shortcut = spec.shortcut};
        }
      }

      return std::nullopt;
    }

    std::string_view commandPaletteTrailingText(rt::CompletionItem const& item)
    {
      if (auto const optMetadata = commandPaletteMetadata(item); optMetadata && !optMetadata->shortcut.empty())
      {
        return optMetadata->shortcut;
      }

      return item.detail;
    }

    std::string commandCompletionSuffix(ShellInteractionModel const& shell)
    {
      auto const& optCompletion = shell.commandCompletion();

      if (!optCompletion || optCompletion->items.empty())
      {
        return {};
      }

      auto const selected = std::clamp<std::int32_t>(
        shell.commandCompletionSelection(), 0, static_cast<std::int32_t>(optCompletion->items.size()) - 1);
      auto const& item = optCompletion->items[static_cast<std::size_t>(selected)];
      auto const replaceBegin = std::min(optCompletion->replaceBegin, shell.commandDraft().size());
      auto const replaceEnd = std::min(optCompletion->replaceEnd, shell.commandDraft().size());
      auto const current = std::string_view{shell.commandDraft()}.substr(replaceBegin, replaceEnd - replaceBegin);

      if (!current.empty() && replaceEnd == shell.commandDraft().size() &&
          rt::startsWithCompletionPrefixInsensitive(item.insertText, current))
      {
        return item.insertText.substr(current.size());
      }

      return {};
    }

    std::vector<SelectableListRow> commandCompletionRows(rt::CompletionResult const& completion,
                                                         std::int32_t const selectedIndex,
                                                         std::int32_t const contentColumns)
    {
      using namespace ftxui;

      std::int32_t categoryColumns = 0;
      std::int32_t trailingColumns = 0;

      for (auto const& item : completion.items)
      {
        if (auto const optMetadata = commandPaletteMetadata(item); optMetadata)
        {
          categoryColumns = std::max(categoryColumns, cellWidth(optMetadata->category));
        }

        trailingColumns = std::max(trailingColumns, cellWidth(commandPaletteTrailingText(item)));
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
          auto categoryPtr =
            fixedText(commandPaletteMetadata(item).value_or(CommandPaletteMetadata{}).category, categoryColumns);
          cells.push_back(selected ? std::move(categoryPtr) : std::move(categoryPtr) | style::accent() | dim);
          cells.push_back(text("  "));
        }

        cells.push_back(text(item.displayText) | flex);

        if (trailingColumns > 0)
        {
          auto trailingPtr = fixedText(commandPaletteTrailingText(item), trailingColumns, CellAlignment::Right);
          cells.push_back(text("  "));
          cells.push_back(selected ? std::move(trailingPtr) : std::move(trailingPtr) | style::accent());
        }

        auto rowPtr = hbox(std::move(cells));

        rows.push_back(SelectableListRow{.elementPtr = std::move(rowPtr), .selected = selected});
      }

      return rows;
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

  ftxui::Element commandPalettePanel(ShellInteractionModel const& shell, std::int32_t columns)
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
      text("/") | style::accent() | bold,
      text(shell.commandDraft()) | bold,
      text(suffix) | dim,
      text("_") | style::accent() | bold,
    }));
    rows.push_back(separator());

    auto const contentColumns = style::popupPanelBodyColumns(columns);

    if (auto const& optCompletion = shell.commandCompletion(); optCompletion && !optCompletion->items.empty())
    {
      rows.push_back(
        selectableList(commandCompletionRows(*optCompletion, shell.commandCompletionSelection(), contentColumns),
                       SelectableListOptions{.focusRow = shell.commandCompletionSelection(), .flex = true}));
    }
    else
    {
      rows.push_back(
        selectableList({}, SelectableListOptions{.emptyText = "No matches", .flex = true, .centerEmpty = true}));
    }

    rows.push_back(separator());
    rows.push_back(style::panelFooterHint(kCommandPaletteFooter));

    return style::popupPanel("Command Palette", vbox(std::move(rows))) | size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
