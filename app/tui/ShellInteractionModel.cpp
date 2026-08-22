// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellInteractionModel.h"

#include "TuiTextCatalog.h"
#include <ao/rt/completion/CompletionResult.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <algorithm>
#include <array>
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
      {.prefix = "filter ",
       .action = CommandAction::QuickFilter,
       .detail = TuiTextId::DetailQuickFilter,
       .category = TuiTextId::CategoryLibrary},
      {.prefix = "presentation ",
       .action = CommandAction::SetPresentation,
       .detail = TuiTextId::DetailTrackView,
       .category = TuiTextId::CategoryView},
      {.prefix = "preset ",
       .action = CommandAction::SetPresentation,
       .detail = TuiTextId::DetailTrackView,
       .category = TuiTextId::CategoryView},
      {.prefix = "view ",
       .action = CommandAction::SetPresentation,
       .detail = TuiTextId::DetailTrackView,
       .category = TuiTextId::CategoryView,
       .optShortcutAction = CommandAction::OpenPresentationPanel},
    });

    constexpr auto kAliasCommands = std::to_array<CommandAliasSpec>({
      {.alias = "lists",
       .action = CommandAction::OpenLists,
       .detail = TuiTextId::DetailChooseList,
       .category = TuiTextId::CategoryLibrary},
      {.alias = "l",
       .action = CommandAction::OpenLists,
       .detail = TuiTextId::DetailChooseList,
       .category = TuiTextId::CategoryLibrary},
      {.alias = "detail",
       .action = CommandAction::OpenDetail,
       .detail = TuiTextId::DetailTrackDetail,
       .category = TuiTextId::CategoryTrack},
      {.alias = "details",
       .action = CommandAction::OpenDetail,
       .detail = TuiTextId::DetailTrackDetail,
       .category = TuiTextId::CategoryTrack},
      {.alias = "d",
       .action = CommandAction::OpenDetail,
       .detail = TuiTextId::DetailTrackDetail,
       .category = TuiTextId::CategoryTrack},
      {.alias = "quality",
       .action = CommandAction::OpenQuality,
       .detail = TuiTextId::DetailAudioPipeline,
       .category = TuiTextId::CategoryAudio},
      {.alias = "audio",
       .action = CommandAction::OpenQuality,
       .detail = TuiTextId::DetailAudioPipeline,
       .category = TuiTextId::CategoryAudio},
      {.alias = "pipeline",
       .action = CommandAction::OpenQuality,
       .detail = TuiTextId::DetailAudioPipeline,
       .category = TuiTextId::CategoryAudio},
      {.alias = "a",
       .action = CommandAction::OpenQuality,
       .detail = TuiTextId::DetailAudioPipeline,
       .category = TuiTextId::CategoryAudio},
      {.alias = "output",
       .action = CommandAction::OpenOutputDevices,
       .detail = TuiTextId::DetailOutputDevice,
       .category = TuiTextId::CategoryAudio},
      {.alias = "outputs",
       .action = CommandAction::OpenOutputDevices,
       .detail = TuiTextId::DetailOutputDevice,
       .category = TuiTextId::CategoryAudio},
      {.alias = "device",
       .action = CommandAction::OpenOutputDevices,
       .detail = TuiTextId::DetailOutputDevice,
       .category = TuiTextId::CategoryAudio},
      {.alias = "devices",
       .action = CommandAction::OpenOutputDevices,
       .detail = TuiTextId::DetailOutputDevice,
       .category = TuiTextId::CategoryAudio},
      {.alias = "o",
       .action = CommandAction::OpenOutputDevices,
       .detail = TuiTextId::DetailOutputDevice,
       .category = TuiTextId::CategoryAudio},
      {.alias = "views",
       .action = CommandAction::OpenPresentationPanel,
       .detail = TuiTextId::DetailChooseView,
       .category = TuiTextId::CategoryView},
      {.alias = "v",
       .action = CommandAction::OpenPresentationPanel,
       .detail = TuiTextId::DetailChooseView,
       .category = TuiTextId::CategoryView},
      {.alias = "notifications",
       .action = CommandAction::OpenNotifications,
       .detail = TuiTextId::DetailNotificationCenter,
       .category = TuiTextId::CategoryStatus},
      {.alias = "notification",
       .action = CommandAction::OpenNotifications,
       .detail = TuiTextId::DetailNotificationCenter,
       .category = TuiTextId::CategoryStatus},
      {.alias = "n",
       .action = CommandAction::OpenNotifications,
       .detail = TuiTextId::DetailNotificationCenter,
       .category = TuiTextId::CategoryStatus},
      {.alias = "close",
       .action = CommandAction::CloseOverlay,
       .detail = TuiTextId::DetailCloseOverlay,
       .category = TuiTextId::CategoryUi},
      {.alias = "hide",
       .action = CommandAction::CloseOverlay,
       .detail = TuiTextId::DetailCloseOverlay,
       .category = TuiTextId::CategoryUi},
      {.alias = "esc",
       .action = CommandAction::CloseOverlay,
       .detail = TuiTextId::DetailCloseOverlay,
       .category = TuiTextId::CategoryUi},
      {.alias = "help",
       .action = CommandAction::ShowHelp,
       .detail = TuiTextId::DetailHelp,
       .category = TuiTextId::CategoryUi},
      {.alias = "h",
       .action = CommandAction::ShowHelp,
       .detail = TuiTextId::DetailHelp,
       .category = TuiTextId::CategoryUi},
      {.alias = "?",
       .action = CommandAction::ShowHelp,
       .detail = TuiTextId::DetailHelp,
       .category = TuiTextId::CategoryUi},
      {.alias = "current",
       .action = CommandAction::RevealCurrentTrack,
       .detail = TuiTextId::DetailNowPlaying,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "now",
       .action = CommandAction::RevealCurrentTrack,
       .detail = TuiTextId::DetailNowPlaying,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "reveal",
       .action = CommandAction::RevealCurrentTrack,
       .detail = TuiTextId::DetailNowPlaying,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "clear",
       .action = CommandAction::ClearFilter,
       .detail = TuiTextId::DetailClearFilter,
       .category = TuiTextId::CategoryLibrary},
      {.alias = "c",
       .action = CommandAction::ClearFilter,
       .detail = TuiTextId::DetailClearFilter,
       .category = TuiTextId::CategoryLibrary},
      {.alias = "reload",
       .action = CommandAction::Reload,
       .detail = TuiTextId::DetailReloadList,
       .category = TuiTextId::CategoryLibrary},
      {.alias = "refresh",
       .action = CommandAction::Reload,
       .detail = TuiTextId::DetailReloadList,
       .category = TuiTextId::CategoryLibrary},
      {.alias = "r",
       .action = CommandAction::Reload,
       .detail = TuiTextId::DetailReloadList,
       .category = TuiTextId::CategoryLibrary},
      {.alias = "play",
       .action = CommandAction::Play,
       .detail = TuiTextId::DetailPlay,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "p",
       .action = CommandAction::Play,
       .detail = TuiTextId::DetailPlay,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "pause",
       .action = CommandAction::TogglePlayback,
       .detail = TuiTextId::DetailPause,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "toggle",
       .action = CommandAction::TogglePlayback,
       .detail = TuiTextId::DetailTogglePlayback,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "space",
       .action = CommandAction::TogglePlayback,
       .detail = TuiTextId::DetailTogglePlayback,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "stop",
       .action = CommandAction::Stop,
       .detail = TuiTextId::DetailStop,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "s",
       .action = CommandAction::Stop,
       .detail = TuiTextId::DetailStop,
       .category = TuiTextId::CategoryPlayback},
      {.alias = "quit",
       .action = CommandAction::Quit,
       .detail = TuiTextId::DetailQuit,
       .category = TuiTextId::CategoryApp},
      {.alias = "q",
       .action = CommandAction::Quit,
       .detail = TuiTextId::DetailQuit,
       .category = TuiTextId::CategoryApp},
    });

    std::string lower(std::string value)
    {
      std::ranges::transform(value, value.begin(), utility::toAsciiLower);
      return value;
    }

    /**
     * @brief Every key that runs a command without the command line.
     *
     * Order decides which key a reader is shown for an action, so the one a
     * shortcut hint should name comes first.
     *
     * Keys that are not commands - seeking, section jumps, volume - are not
     * here: they are answered where they are pressed and are named nowhere
     * else, so there is nothing for a second declaration to drift from.
     */
    constexpr auto kKeyBindings = std::to_array<KeyBindingSpec>({
      {.key = "q", .action = CommandAction::Quit},
      {.key = "l", .action = CommandAction::OpenLists},
      {.key = "d", .action = CommandAction::OpenDetail},
      {.key = "a", .action = CommandAction::OpenQuality},
      {.key = "o", .action = CommandAction::OpenOutputDevices},
      {.key = "v", .action = CommandAction::OpenPresentationPanel},
      {.key = "n", .action = CommandAction::OpenNotifications},
      {.key = "?", .action = CommandAction::ShowHelp},
      {.key = "Ctrl-L", .action = CommandAction::RevealCurrentTrack},
      {.key = "c", .action = CommandAction::ClearFilter},
      {.key = "r", .action = CommandAction::Reload},
      {.key = "Enter", .action = CommandAction::Play},
      {.key = "p", .action = CommandAction::Play},
      {.key = "Space", .action = CommandAction::TogglePlayback},
      {.key = "s", .action = CommandAction::Stop},
      {.key = "Esc", .action = CommandAction::CloseOverlay},
    });
  } // namespace

  std::span<KeyBindingSpec const> keyBindingSpecs()
  {
    return kKeyBindings;
  }

  std::string_view shortcutFor(CommandAction const action)
  {
    // std::array's iterator is a raw pointer in libstdc++ but a class type in
    // MSVC's STL, so spelling it as a pointer does not compile on Windows.
    // NOLINTNEXTLINE(readability-qualified-auto)
    auto const it = std::ranges::find(kKeyBindings, action, &KeyBindingSpec::action);
    return it == kKeyBindings.end() ? std::string_view{} : it->key;
  }

  std::span<CommandPrefixSpec const> commandPrefixSpecs()
  {
    return kPrefixCommands;
  }

  std::span<CommandAliasSpec const> commandAliasSpecs()
  {
    return kAliasCommands;
  }

  bool isModalOverlay(Overlay const overlay) noexcept
  {
    switch (overlay)
    {
      case Overlay::None:
      case Overlay::DetailPanel: return false;
      case Overlay::ListChooser:
      case Overlay::QualityPanel:
      case Overlay::OutputDevices:
      case Overlay::PresentationPanel:
      case Overlay::Notifications:
      case Overlay::Help: return true;
    }

    return true;
  }

  std::optional<Command> parseCommand(std::string_view input)
  {
    auto value = utility::trim(input);

    if (!value.empty() && value.front() == ':')
    {
      value.remove_prefix(1);
      value = utility::trim(value);
    }

    if (value.empty())
    {
      return std::nullopt;
    }

    auto command = lower(std::string{value});

    for (auto const& prefixCommand : kPrefixCommands)
    {
      if (command.starts_with(prefixCommand.prefix))
      {
        return Command{
          .action = prefixCommand.action,
          .argument = std::string{utility::trim(value.substr(prefixCommand.prefix.size()))},
        };
      }
    }

    // NOLINTNEXTLINE(readability-qualified-auto) -- std::array iterator representations differ across libraries.
    auto const aliasIt = std::ranges::find_if(
      kAliasCommands, [&](CommandAliasSpec const& aliasCommand) { return command == aliasCommand.alias; });

    if (aliasIt != kAliasCommands.end())
    {
      return Command{.action = aliasIt->action};
    }

    return std::nullopt;
  }

  std::string_view overlayLabel(TuiTextCatalog const& textCatalog, Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return textCatalog.text(TuiTextId::OverlayTracks);
      case Overlay::ListChooser: return textCatalog.text(TuiTextId::OverlayLists);
      case Overlay::DetailPanel: return textCatalog.text(TuiTextId::OverlayDetail);
      case Overlay::QualityPanel: return textCatalog.text(TuiTextId::OverlayPipeline);
      case Overlay::OutputDevices: return textCatalog.text(TuiTextId::OverlayOutput);
      case Overlay::PresentationPanel: return textCatalog.text(TuiTextId::OverlayViews);
      case Overlay::Notifications: return textCatalog.text(TuiTextId::OverlayNotifications);
      case Overlay::Help: return textCatalog.text(TuiTextId::OverlayHelp);
    }

    return textCatalog.text(TuiTextId::OverlayTracks);
  }

  std::string_view overlayHint(TuiTextCatalog const& textCatalog, Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return {};
      case Overlay::ListChooser: return textCatalog.text(TuiTextId::HintLists);
      case Overlay::DetailPanel: return textCatalog.text(TuiTextId::HintDetail);
      case Overlay::QualityPanel: return textCatalog.text(TuiTextId::HintPipeline);
      case Overlay::OutputDevices: return textCatalog.text(TuiTextId::HintOutput);
      case Overlay::PresentationPanel: return textCatalog.text(TuiTextId::HintViews);
      case Overlay::Notifications: return textCatalog.text(TuiTextId::HintNotifications);
      case Overlay::Help: return textCatalog.text(TuiTextId::HintHelp);
    }

    return {};
  }

  bool ShellInteractionModel::isInputActive() const noexcept
  {
    return _inputMode != ShellInputMode::None;
  }

  ShellInputMode ShellInteractionModel::inputMode() const noexcept
  {
    return _inputMode;
  }

  std::string const& ShellInteractionModel::inputDraft() const noexcept
  {
    return _inputDraft;
  }

  bool ShellInteractionModel::isInputTouched() const noexcept
  {
    return _inputTouched;
  }

  std::optional<rt::CompletionResult> const& ShellInteractionModel::commandCompletion() const noexcept
  {
    return _completion.result();
  }

  std::int32_t ShellInteractionModel::commandCompletionSelection() const noexcept
  {
    return _completion.selection();
  }

  Overlay ShellInteractionModel::overlay() const noexcept
  {
    return _overlay;
  }

  void ShellInteractionModel::beginInput(ShellInputMode const mode, std::string draft)
  {
    _inputMode = mode;
    _inputDraft = std::move(draft);
    _inputTouched = !_inputDraft.empty();
    clearCommandCompletion();
  }

  void ShellInteractionModel::appendInputText(std::string_view const text)
  {
    if (text.empty())
    {
      return;
    }

    _inputDraft.append(text);
    _inputTouched = true;
  }

  void ShellInteractionModel::backspaceInput()
  {
    if (_inputDraft.empty())
    {
      return;
    }

    _inputTouched = true;
    auto const boundaryRes = utility::previousUtf8GraphemeBoundary(_inputDraft);

    if (boundaryRes)
    {
      _inputDraft.resize(*boundaryRes);
      return;
    }

    // Terminal input is expected to be valid UTF-8. Preserve the former
    // code-point fallback if an invalid byte sequence or ICU failure reaches
    // this UI-only boundary so Backspace still makes progress.
    constexpr unsigned int kUtf8ContinuationMask = 0xC0U;
    constexpr unsigned int kUtf8ContinuationTag = 0x80U;

    while (!_inputDraft.empty() &&
           (static_cast<unsigned char>(_inputDraft.back()) & kUtf8ContinuationMask) == kUtf8ContinuationTag)
    {
      _inputDraft.pop_back();
    }

    if (!_inputDraft.empty())
    {
      _inputDraft.pop_back();
    }
  }

  void ShellInteractionModel::closeInput()
  {
    _inputMode = ShellInputMode::None;
    _inputDraft.clear();
    _inputTouched = false;
    clearCommandCompletion();
  }

  void ShellInteractionModel::setCommandCompletion(std::optional<rt::CompletionResult> optCompletion)
  {
    _completion.set(std::move(optCompletion));
  }

  bool ShellInteractionModel::moveCommandCompletion(std::int32_t const delta)
  {
    return _completion.moveSelection(delta);
  }

  bool ShellInteractionModel::moveCommandCompletionByPage(std::int32_t const delta)
  {
    return _completion.moveSelectionByPage(delta);
  }

  bool ShellInteractionModel::applyCommandCompletion()
  {
    if (!_completion.applyTo(_inputDraft))
    {
      return false;
    }

    _inputTouched = true;
    return true;
  }

  void ShellInteractionModel::clearCommandCompletion()
  {
    _completion.clear();
  }

  void ShellInteractionModel::openOverlay(Overlay overlay) noexcept
  {
    _overlay = overlay;
  }

  void ShellInteractionModel::closeOverlay() noexcept
  {
    _overlay = Overlay::None;
  }
} // namespace ao::tui
