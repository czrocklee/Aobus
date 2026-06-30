// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellModel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    struct PrefixCommand final
    {
      std::string_view prefix;
      CommandAction action;
    };

    struct AliasCommand final
    {
      std::string_view alias;
      CommandAction action;
    };

    constexpr auto kPrefixCommands = std::to_array<PrefixCommand>({
      {.prefix = "filter ", .action = CommandAction::QuickFilter},
      {.prefix = "presentation ", .action = CommandAction::SetPresentation},
      {.prefix = "preset ", .action = CommandAction::SetPresentation},
      {.prefix = "view ", .action = CommandAction::SetPresentation},
    });

    constexpr auto kAliasCommands = std::to_array<AliasCommand>({
      {.alias = "lists", .action = CommandAction::OpenLists},
      {.alias = "l", .action = CommandAction::OpenLists},
      {.alias = "detail", .action = CommandAction::OpenDetail},
      {.alias = "details", .action = CommandAction::OpenDetail},
      {.alias = "d", .action = CommandAction::OpenDetail},
      {.alias = "quality", .action = CommandAction::OpenQuality},
      {.alias = "audio", .action = CommandAction::OpenQuality},
      {.alias = "pipeline", .action = CommandAction::OpenQuality},
      {.alias = "a", .action = CommandAction::OpenQuality},
      {.alias = "output", .action = CommandAction::OpenOutputDevices},
      {.alias = "outputs", .action = CommandAction::OpenOutputDevices},
      {.alias = "device", .action = CommandAction::OpenOutputDevices},
      {.alias = "devices", .action = CommandAction::OpenOutputDevices},
      {.alias = "o", .action = CommandAction::OpenOutputDevices},
      {.alias = "close", .action = CommandAction::CloseOverlay},
      {.alias = "hide", .action = CommandAction::CloseOverlay},
      {.alias = "esc", .action = CommandAction::CloseOverlay},
      {.alias = "help", .action = CommandAction::ShowHelp},
      {.alias = "h", .action = CommandAction::ShowHelp},
      {.alias = "?", .action = CommandAction::ShowHelp},
      {.alias = "current", .action = CommandAction::RevealCurrentTrack},
      {.alias = "now", .action = CommandAction::RevealCurrentTrack},
      {.alias = "reveal", .action = CommandAction::RevealCurrentTrack},
      {.alias = "clear", .action = CommandAction::ClearFilter},
      {.alias = "c", .action = CommandAction::ClearFilter},
      {.alias = "reload", .action = CommandAction::Reload},
      {.alias = "refresh", .action = CommandAction::Reload},
      {.alias = "r", .action = CommandAction::Reload},
      {.alias = "play", .action = CommandAction::Play},
      {.alias = "p", .action = CommandAction::Play},
      {.alias = "pause", .action = CommandAction::TogglePlayback},
      {.alias = "toggle", .action = CommandAction::TogglePlayback},
      {.alias = "space", .action = CommandAction::TogglePlayback},
      {.alias = "stop", .action = CommandAction::Stop},
      {.alias = "s", .action = CommandAction::Stop},
      {.alias = "quit", .action = CommandAction::Quit},
      {.alias = "q", .action = CommandAction::Quit},
    });

    std::string trim(std::string_view value)
    {
      auto const* begin = value.begin();
      auto const* end = value.end();

      while (begin != end && std::isspace(static_cast<unsigned char>(*begin)) != 0)
      {
        ++begin;
      }

      while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0)
      {
        --end;
      }

      return {begin, end};
    }

    std::string lower(std::string value)
    {
      std::ranges::transform(
        value, value.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
      return value;
    }
  } // namespace

  Command parseCommand(std::string_view input)
  {
    auto value = trim(input);

    if (!value.empty() && (value.front() == '/' || value.front() == ':'))
    {
      value.erase(value.begin());
      value = trim(value);
    }

    auto command = lower(value);

    for (auto const& prefixCommand : kPrefixCommands)
    {
      if (command.starts_with(prefixCommand.prefix))
      {
        return {.action = prefixCommand.action, .argument = trim(value.substr(prefixCommand.prefix.size()))};
      }
    }

    auto const* const aliasIt = std::ranges::find_if(
      kAliasCommands, [&](AliasCommand const& aliasCommand) { return command == aliasCommand.alias; });

    if (aliasIt != kAliasCommands.end())
    {
      return {.action = aliasIt->action};
    }

    return {.action = CommandAction::QuickFilter, .argument = value};
  }

  std::string overlayLabel(Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return "Tracks";
      case Overlay::ListChooser: return "Lists";
      case Overlay::DetailPanel: return "Detail";
      case Overlay::QualityPanel: return "Quality";
      case Overlay::OutputDevices: return "Output";
      case Overlay::Help: return "Help";
    }

    return "Tracks";
  }

  bool ShellModel::commandActive() const noexcept
  {
    return _commandActive;
  }

  std::string const& ShellModel::commandDraft() const noexcept
  {
    return _commandDraft;
  }

  Overlay ShellModel::overlay() const noexcept
  {
    return _overlay;
  }

  void ShellModel::beginCommand(std::string draft)
  {
    _commandActive = true;
    _commandDraft = std::move(draft);
  }

  void ShellModel::appendCommandText(std::string_view text)
  {
    _commandDraft.append(text);
  }

  void ShellModel::backspaceCommand()
  {
    constexpr unsigned int kUtf8ContinuationMask = 0xC0U;
    constexpr unsigned int kUtf8ContinuationTag = 0x80U;

    while (!_commandDraft.empty() &&
           (static_cast<unsigned char>(_commandDraft.back()) & kUtf8ContinuationMask) == kUtf8ContinuationTag)
    {
      _commandDraft.pop_back();
    }

    if (!_commandDraft.empty())
    {
      _commandDraft.pop_back();
    }
  }

  void ShellModel::cancelCommand()
  {
    _commandActive = false;
    _commandDraft.clear();
  }

  Command ShellModel::submitCommand()
  {
    auto command = parseCommand(_commandDraft);
    _commandActive = false;
    _commandDraft.clear();
    return command;
  }

  void ShellModel::openOverlay(Overlay overlay) noexcept
  {
    _overlay = overlay;
  }

  void ShellModel::closeOverlay() noexcept
  {
    _overlay = Overlay::None;
  }
} // namespace ao::tui
