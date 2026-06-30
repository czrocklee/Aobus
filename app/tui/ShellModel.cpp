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
    struct PrefixCommand final
    {
      std::string_view prefix;
      CommandAction action;
    };

    constexpr auto kPrefixCommands = std::array{
      PrefixCommand{.prefix = "filter ", .action = CommandAction::QuickFilter},
      PrefixCommand{.prefix = "presentation ", .action = CommandAction::SetPresentation},
      PrefixCommand{.prefix = "preset ", .action = CommandAction::SetPresentation},
      PrefixCommand{.prefix = "view ", .action = CommandAction::SetPresentation},
    };

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

    if (command == "lists" || command == "l")
    {
      return {.action = CommandAction::OpenLists};
    }

    if (command == "detail" || command == "details" || command == "d")
    {
      return {.action = CommandAction::OpenDetail};
    }

    if (command == "quality" || command == "audio" || command == "pipeline" || command == "a")
    {
      return {.action = CommandAction::OpenQuality};
    }

    if (command == "close" || command == "hide" || command == "esc")
    {
      return {.action = CommandAction::CloseOverlay};
    }

    if (command == "help" || command == "h" || command == "?")
    {
      return {.action = CommandAction::ShowHelp};
    }

    if (command == "current" || command == "now" || command == "reveal")
    {
      return {.action = CommandAction::RevealCurrentTrack};
    }

    if (command == "clear" || command == "c")
    {
      return {.action = CommandAction::ClearFilter};
    }

    if (command == "reload" || command == "refresh" || command == "r")
    {
      return {.action = CommandAction::Reload};
    }

    if (command == "play" || command == "p")
    {
      return {.action = CommandAction::Play};
    }

    if (command == "pause" || command == "toggle" || command == "space")
    {
      return {.action = CommandAction::TogglePlayback};
    }

    if (command == "stop" || command == "s")
    {
      return {.action = CommandAction::Stop};
    }

    if (command == "quit" || command == "q")
    {
      return {.action = CommandAction::Quit};
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
