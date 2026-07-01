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
      {.prefix = "filter ", .action = CommandAction::QuickFilter, .detail = "quick filter"},
      {.prefix = "presentation ", .action = CommandAction::SetPresentation, .detail = "track view"},
      {.prefix = "preset ", .action = CommandAction::SetPresentation, .detail = "track view"},
      {.prefix = "view ", .action = CommandAction::SetPresentation, .detail = "track view"},
    });

    constexpr auto kAliasCommands = std::to_array<CommandAliasSpec>({
      {.alias = "lists", .action = CommandAction::OpenLists, .detail = "choose list"},
      {.alias = "l", .action = CommandAction::OpenLists, .detail = "choose list"},
      {.alias = "detail", .action = CommandAction::OpenDetail, .detail = "track detail"},
      {.alias = "details", .action = CommandAction::OpenDetail, .detail = "track detail"},
      {.alias = "d", .action = CommandAction::OpenDetail, .detail = "track detail"},
      {.alias = "quality", .action = CommandAction::OpenQuality, .detail = "audio quality"},
      {.alias = "audio", .action = CommandAction::OpenQuality, .detail = "audio quality"},
      {.alias = "pipeline", .action = CommandAction::OpenQuality, .detail = "audio quality"},
      {.alias = "a", .action = CommandAction::OpenQuality, .detail = "audio quality"},
      {.alias = "output", .action = CommandAction::OpenOutputDevices, .detail = "output device"},
      {.alias = "outputs", .action = CommandAction::OpenOutputDevices, .detail = "output device"},
      {.alias = "device", .action = CommandAction::OpenOutputDevices, .detail = "output device"},
      {.alias = "devices", .action = CommandAction::OpenOutputDevices, .detail = "output device"},
      {.alias = "o", .action = CommandAction::OpenOutputDevices, .detail = "output device"},
      {.alias = "close", .action = CommandAction::CloseOverlay, .detail = "close overlay"},
      {.alias = "hide", .action = CommandAction::CloseOverlay, .detail = "close overlay"},
      {.alias = "esc", .action = CommandAction::CloseOverlay, .detail = "close overlay"},
      {.alias = "help", .action = CommandAction::ShowHelp, .detail = "help"},
      {.alias = "h", .action = CommandAction::ShowHelp, .detail = "help"},
      {.alias = "?", .action = CommandAction::ShowHelp, .detail = "help"},
      {.alias = "current", .action = CommandAction::RevealCurrentTrack, .detail = "now playing"},
      {.alias = "now", .action = CommandAction::RevealCurrentTrack, .detail = "now playing"},
      {.alias = "reveal", .action = CommandAction::RevealCurrentTrack, .detail = "now playing"},
      {.alias = "clear", .action = CommandAction::ClearFilter, .detail = "clear filter"},
      {.alias = "c", .action = CommandAction::ClearFilter, .detail = "clear filter"},
      {.alias = "reload", .action = CommandAction::Reload, .detail = "reload list"},
      {.alias = "refresh", .action = CommandAction::Reload, .detail = "reload list"},
      {.alias = "r", .action = CommandAction::Reload, .detail = "reload list"},
      {.alias = "play", .action = CommandAction::Play, .detail = "play"},
      {.alias = "p", .action = CommandAction::Play, .detail = "play"},
      {.alias = "pause", .action = CommandAction::TogglePlayback, .detail = "pause"},
      {.alias = "toggle", .action = CommandAction::TogglePlayback, .detail = "toggle playback"},
      {.alias = "space", .action = CommandAction::TogglePlayback, .detail = "toggle playback"},
      {.alias = "stop", .action = CommandAction::Stop, .detail = "stop"},
      {.alias = "s", .action = CommandAction::Stop, .detail = "stop"},
      {.alias = "quit", .action = CommandAction::Quit, .detail = "quit"},
      {.alias = "q", .action = CommandAction::Quit, .detail = "quit"},
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
