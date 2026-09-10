// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "GoToMenu.h"

#include "Command.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  std::span<CommandAliasSpec const> goToCommands()
  {
    static auto const kCommands = []
    {
      auto result = std::vector<CommandAliasSpec>{};

      for (auto const& command : commandAliasSpecs())
      {
        if (!command.goToKey.empty())
        {
          result.push_back(command);
        }
      }

      return result;
    }();
    return kCommands;
  }

  bool canActivateGoTo(GoToMenuState const& state, CommandAction const action)
  {
    switch (action)
    {
      case CommandAction::RevealCurrentTrack:
      case CommandAction::OpenCurrentAlbum: return state.nowPlaying.trackId != kInvalidTrackId;
      case CommandAction::OpenCurrentArtist:
        return state.nowPlaying.trackId != kInvalidTrackId && !state.nowPlaying.artist.empty();
      case CommandAction::Back: return state.canGoBack;
      case CommandAction::Forward: return state.canGoForward;
      default: return false;
    }
  }

  ftxui::Element goToHintBar(i18n::MessageCatalog const& textCatalog,
                             GoToMenuState const& state,
                             std::int32_t const columns,
                             GoToMenuHitRegions* const hitRegions)
  {
    using namespace ftxui;
    auto const commands = goToCommands();
    // Reserve every suffix, Escape, and separators before budgeting translated labels.
    auto keyColumns = cellWidth("Esc");

    for (auto const& command : commands)
    {
      keyColumns += cellWidth(command.goToKey);
    }

    auto const itemCount = static_cast<std::int32_t>(commands.size()) + 1;
    auto const separatorColumns = (itemCount - 1) * cellWidth(" · ");
    auto const labelColumns = std::max(0, ((columns - keyColumns - separatorColumns) / itemCount) - 1);
    auto parts = Elements{filler() | xflex};

    if (hitRegions != nullptr)
    {
      *hitRegions = {.state = state};
    }

    auto chip = [&](std::string const& key, i18n::MessageId const labelId)
    {
      auto const label = ellipsizeToCellWidth(i18n::requiredText(textCatalog, labelId), labelColumns);
      return label.empty() ? text(key) | style::accent() | bold : style::shortcutChip(key, label);
    };
    auto shortLabel = [](CommandAliasSpec const& command)
    {
      using enum i18n::MessageId;

      switch (command.action)
      {
        case CommandAction::RevealCurrentTrack: return TuiShellCategoryTrack;
        case CommandAction::OpenCurrentArtist: return TrackFieldArtist;
        case CommandAction::OpenCurrentAlbum: return TrackFieldAlbum;
        case CommandAction::Back: return TuiGoToBack;
        case CommandAction::Forward: return TuiGoToForward;
        default: return command.detail;
      }
    };

    for (auto const& command : commands)
    {
      auto chipPtr = chip(std::string{command.goToKey}, shortLabel(command));

      if (!canActivateGoTo(state, command.action))
      {
        chipPtr = std::move(chipPtr) | dim;
      }

      if (hitRegions != nullptr)
      {
        hitRegions->rows.push_back({.action = command.action});
        chipPtr = std::move(chipPtr) | reflect(hitRegions->rows.back().box);
      }

      parts.push_back(std::move(chipPtr));
      parts.push_back(text(" · ") | dim);
    }

    auto cancelPtr = chip("Esc", i18n::MessageId::TuiGoToCancel);

    if (hitRegions != nullptr)
    {
      cancelPtr = std::move(cancelPtr) | reflect(hitRegions->cancelBox);
    }

    parts.push_back(std::move(cancelPtr));
    return hbox(std::move(parts)) | clear_under;
  }

  ftxui::Element goToMenu(i18n::MessageCatalog const& textCatalog,
                          GoToMenuState const& state,
                          std::int32_t const selected,
                          std::int32_t const columns,
                          GoToMenuHitRegions* const hitRegions)
  {
    using namespace ftxui;

    if (hitRegions != nullptr)
    {
      *hitRegions = {.state = state};
    }

    auto const bodyColumns = style::popupPanelBodyColumns(columns);
    auto rows = Elements{
      text(ellipsizeToCellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TuiGoToNowPlaying), bodyColumns)) |
      bold};
    std::int32_t index = 0;

    for (auto const& command : goToCommands())
    {
      if (command.action == CommandAction::Back)
      {
        rows.push_back(separator());
        rows.push_back(
          text(ellipsizeToCellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TuiGoToHistory), bodyColumns)) |
          bold);
      }

      auto const enabled = canActivateGoTo(state, command.action);
      auto const detail = command.action == CommandAction::RevealCurrentTrack
                            ? i18n::MessageId::TuiShellHelpCurrentTrack
                            : command.detail;
      auto label = std::string{command.goToKey} + "  " + std::string{i18n::requiredText(textCatalog, detail)};
      auto rowPtr = text(fitCellText(ellipsizeToCellWidth(label, bodyColumns), bodyColumns));

      if (selected == index)
      {
        rowPtr = std::move(rowPtr) | style::selected() | focus;
      }

      if (!enabled)
      {
        rowPtr = std::move(rowPtr) | dim;
      }

      if (hitRegions != nullptr)
      {
        hitRegions->rows.push_back({.action = command.action});
        rowPtr = std::move(rowPtr) | reflect(hitRegions->rows.back().box);
      }

      rows.push_back(std::move(rowPtr));
      ++index;
    }

    rows.push_back(separator());
    rows.push_back(
      text(ellipsizeToCellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TuiGoToHint), bodyColumns)) | dim);
    return style::titledPanel(i18n::requiredText(textCatalog, i18n::MessageId::TuiGoToTitle),
                              style::scrollablePanelBody(vbox(std::move(rows)) | vscroll_indicator | yframe)) |
           size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
