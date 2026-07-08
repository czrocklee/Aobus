// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellModel.h"

#include <ao/rt/completion/CompletionResult.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    constexpr auto kPrefixCommands = std::to_array<CommandPrefixSpec>({
      {.prefix = "filter ", .action = CommandAction::QuickFilter, .detail = "quick filter", .category = "library"},
      {.prefix = "presentation ", .action = CommandAction::SetPresentation, .detail = "track view", .category = "view"},
      {.prefix = "preset ", .action = CommandAction::SetPresentation, .detail = "track view", .category = "view"},
      {.prefix = "view ",
       .action = CommandAction::SetPresentation,
       .detail = "track view",
       .category = "view",
       .shortcut = "v"},
    });

    constexpr auto kAliasCommands = std::to_array<CommandAliasSpec>({
      {.alias = "lists",
       .action = CommandAction::OpenLists,
       .detail = "choose list",
       .category = "library",
       .shortcut = "l"},
      {.alias = "l",
       .action = CommandAction::OpenLists,
       .detail = "choose list",
       .category = "library",
       .shortcut = "l"},
      {.alias = "detail",
       .action = CommandAction::OpenDetail,
       .detail = "track detail",
       .category = "track",
       .shortcut = "d"},
      {.alias = "details",
       .action = CommandAction::OpenDetail,
       .detail = "track detail",
       .category = "track",
       .shortcut = "d"},
      {.alias = "d",
       .action = CommandAction::OpenDetail,
       .detail = "track detail",
       .category = "track",
       .shortcut = "d"},
      {.alias = "quality",
       .action = CommandAction::OpenQuality,
       .detail = "audio pipeline",
       .category = "audio",
       .shortcut = "a"},
      {.alias = "audio",
       .action = CommandAction::OpenQuality,
       .detail = "audio pipeline",
       .category = "audio",
       .shortcut = "a"},
      {.alias = "pipeline",
       .action = CommandAction::OpenQuality,
       .detail = "audio pipeline",
       .category = "audio",
       .shortcut = "a"},
      {.alias = "a",
       .action = CommandAction::OpenQuality,
       .detail = "audio pipeline",
       .category = "audio",
       .shortcut = "a"},
      {.alias = "output",
       .action = CommandAction::OpenOutputDevices,
       .detail = "output device",
       .category = "audio",
       .shortcut = "o"},
      {.alias = "outputs",
       .action = CommandAction::OpenOutputDevices,
       .detail = "output device",
       .category = "audio",
       .shortcut = "o"},
      {.alias = "device",
       .action = CommandAction::OpenOutputDevices,
       .detail = "output device",
       .category = "audio",
       .shortcut = "o"},
      {.alias = "devices",
       .action = CommandAction::OpenOutputDevices,
       .detail = "output device",
       .category = "audio",
       .shortcut = "o"},
      {.alias = "o",
       .action = CommandAction::OpenOutputDevices,
       .detail = "output device",
       .category = "audio",
       .shortcut = "o"},
      {.alias = "views",
       .action = CommandAction::OpenPresentationPanel,
       .detail = "choose view",
       .category = "view",
       .shortcut = "v"},
      {.alias = "v",
       .action = CommandAction::OpenPresentationPanel,
       .detail = "choose view",
       .category = "view",
       .shortcut = "v"},
      {.alias = "notifications",
       .action = CommandAction::OpenNotifications,
       .detail = "notification center",
       .category = "status",
       .shortcut = "n"},
      {.alias = "notification",
       .action = CommandAction::OpenNotifications,
       .detail = "notification center",
       .category = "status",
       .shortcut = "n"},
      {.alias = "n",
       .action = CommandAction::OpenNotifications,
       .detail = "notification center",
       .category = "status",
       .shortcut = "n"},
      {.alias = "close",
       .action = CommandAction::CloseOverlay,
       .detail = "close overlay",
       .category = "ui",
       .shortcut = "Esc"},
      {.alias = "hide",
       .action = CommandAction::CloseOverlay,
       .detail = "close overlay",
       .category = "ui",
       .shortcut = "Esc"},
      {.alias = "esc",
       .action = CommandAction::CloseOverlay,
       .detail = "close overlay",
       .category = "ui",
       .shortcut = "Esc"},
      {.alias = "help", .action = CommandAction::ShowHelp, .detail = "help", .category = "ui", .shortcut = "?"},
      {.alias = "h", .action = CommandAction::ShowHelp, .detail = "help", .category = "ui", .shortcut = "?"},
      {.alias = "?", .action = CommandAction::ShowHelp, .detail = "help", .category = "ui", .shortcut = "?"},
      {.alias = "current",
       .action = CommandAction::RevealCurrentTrack,
       .detail = "now playing",
       .category = "playback",
       .shortcut = "Ctrl-L"},
      {.alias = "now",
       .action = CommandAction::RevealCurrentTrack,
       .detail = "now playing",
       .category = "playback",
       .shortcut = "Ctrl-L"},
      {.alias = "reveal",
       .action = CommandAction::RevealCurrentTrack,
       .detail = "now playing",
       .category = "playback",
       .shortcut = "Ctrl-L"},
      {.alias = "clear",
       .action = CommandAction::ClearFilter,
       .detail = "clear filter",
       .category = "library",
       .shortcut = "c"},
      {.alias = "c",
       .action = CommandAction::ClearFilter,
       .detail = "clear filter",
       .category = "library",
       .shortcut = "c"},
      {.alias = "reload",
       .action = CommandAction::Reload,
       .detail = "reload list",
       .category = "library",
       .shortcut = "r"},
      {.alias = "refresh",
       .action = CommandAction::Reload,
       .detail = "reload list",
       .category = "library",
       .shortcut = "r"},
      {.alias = "r", .action = CommandAction::Reload, .detail = "reload list", .category = "library", .shortcut = "r"},
      {.alias = "play", .action = CommandAction::Play, .detail = "play", .category = "playback", .shortcut = "Enter"},
      {.alias = "p", .action = CommandAction::Play, .detail = "play", .category = "playback", .shortcut = "p"},
      {.alias = "pause",
       .action = CommandAction::TogglePlayback,
       .detail = "pause",
       .category = "playback",
       .shortcut = "Space"},
      {.alias = "toggle",
       .action = CommandAction::TogglePlayback,
       .detail = "toggle playback",
       .category = "playback",
       .shortcut = "Space"},
      {.alias = "space",
       .action = CommandAction::TogglePlayback,
       .detail = "toggle playback",
       .category = "playback",
       .shortcut = "Space"},
      {.alias = "stop", .action = CommandAction::Stop, .detail = "stop", .category = "playback", .shortcut = "s"},
      {.alias = "s", .action = CommandAction::Stop, .detail = "stop", .category = "playback", .shortcut = "s"},
      {.alias = "quit", .action = CommandAction::Quit, .detail = "quit", .category = "app", .shortcut = "q"},
      {.alias = "q", .action = CommandAction::Quit, .detail = "quit", .category = "app", .shortcut = "q"},
    });

    constexpr std::string_view kWorkspaceHint =
      "/ command  l lists  v view  n notif  d detail  a pipeline  o output  { } groups  Ctrl-L current  q quit";

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

  std::span<CommandPrefixSpec const> commandPrefixSpecs()
  {
    return kPrefixCommands;
  }

  std::span<CommandAliasSpec const> commandAliasSpecs()
  {
    return kAliasCommands;
  }

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
      kAliasCommands, [&](CommandAliasSpec const& aliasCommand) { return command == aliasCommand.alias; });

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
      case Overlay::QualityPanel: return "Pipeline";
      case Overlay::OutputDevices: return "Output";
      case Overlay::PresentationPanel: return "Views";
      case Overlay::Notifications: return "Notifications";
      case Overlay::Help: return "Help";
    }

    return "Tracks";
  }

  std::string_view overlayHint(Overlay const overlay)
  {
    using namespace std::literals;

    switch (overlay)
    {
      case Overlay::None: return kWorkspaceHint;
      case Overlay::ListChooser: return "l toggle  Enter open  Esc close"sv;
      case Overlay::DetailPanel: return "d toggle  Esc close"sv;
      case Overlay::QualityPanel: return "a toggle  Esc close"sv;
      case Overlay::OutputDevices: return "o toggle  Enter select  Esc close"sv;
      case Overlay::PresentationPanel: return "v toggle  Enter select  Esc close"sv;
      case Overlay::Notifications: return "n toggle  x hide compact  Esc close"sv;
      case Overlay::Help: return "Esc close"sv;
    }

    return kWorkspaceHint;
  }

  bool ShellModel::isCommandActive() const noexcept
  {
    return _commandActive;
  }

  std::string const& ShellModel::commandDraft() const noexcept
  {
    return _commandDraft;
  }

  std::optional<rt::CompletionResult> const& ShellModel::commandCompletion() const noexcept
  {
    return _completion.result();
  }

  std::int32_t ShellModel::commandCompletionSelection() const noexcept
  {
    return _completion.selection();
  }

  Overlay ShellModel::overlay() const noexcept
  {
    return _overlay;
  }

  void ShellModel::beginCommand(std::string draft)
  {
    _commandActive = true;
    _commandDraft = std::move(draft);
    clearCommandCompletion();
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
    clearCommandCompletion();
  }

  Command ShellModel::submitCommand()
  {
    auto command = parseCommand(_commandDraft);
    _commandActive = false;
    _commandDraft.clear();
    clearCommandCompletion();
    return command;
  }

  void ShellModel::setCommandCompletion(std::optional<rt::CompletionResult> optCompletion)
  {
    _completion.set(std::move(optCompletion));
  }

  bool ShellModel::moveCommandCompletion(std::int32_t const delta)
  {
    return _completion.moveSelection(delta);
  }

  bool ShellModel::applyCommandCompletion()
  {
    return _completion.applyTo(_commandDraft);
  }

  void ShellModel::clearCommandCompletion()
  {
    _completion.clear();
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
