// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellInteractionModel.h"

#include "TuiKeymap.h"
#include "TuiText.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <ftxui/component/event.hpp>

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
       .detail = i18n::MessageId::TuiShellDetailQuickFilter,
       .category = i18n::MessageId::TuiShellCategoryLibrary,
       .optShortcutAction = TuiKeyAction::OpenQuickFilter},
      {.prefix = "presentation ",
       .action = CommandAction::SetPresentation,
       .detail = i18n::MessageId::TuiShellDetailTrackView,
       .category = i18n::MessageId::TuiShellCategoryView},
      {.prefix = "preset ",
       .action = CommandAction::SetPresentation,
       .detail = i18n::MessageId::TuiShellDetailTrackView,
       .category = i18n::MessageId::TuiShellCategoryView},
      {.prefix = "view ",
       .action = CommandAction::SetPresentation,
       .detail = i18n::MessageId::TuiShellDetailTrackView,
       .category = i18n::MessageId::TuiShellCategoryView,
       .optShortcutAction = TuiKeyAction::TogglePresentations},
    });

    constexpr auto kAliasCommands = std::to_array<CommandAliasSpec>({
      {.alias = "lists",
       .action = CommandAction::OpenLists,
       .detail = i18n::MessageId::TuiShellDetailChooseList,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "l",
       .action = CommandAction::OpenLists,
       .detail = i18n::MessageId::TuiShellDetailChooseList,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "detail",
       .action = CommandAction::OpenDetail,
       .detail = i18n::MessageId::TuiShellDetailTrackDetail,
       .category = i18n::MessageId::TuiShellCategoryTrack},
      {.alias = "details",
       .action = CommandAction::OpenDetail,
       .detail = i18n::MessageId::TuiShellDetailTrackDetail,
       .category = i18n::MessageId::TuiShellCategoryTrack},
      {.alias = "d",
       .action = CommandAction::OpenDetail,
       .detail = i18n::MessageId::TuiShellDetailTrackDetail,
       .category = i18n::MessageId::TuiShellCategoryTrack},
      {.alias = "quality",
       .action = CommandAction::OpenQuality,
       .detail = i18n::MessageId::TuiShellDetailAudioPipeline,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "audio",
       .action = CommandAction::OpenQuality,
       .detail = i18n::MessageId::TuiShellDetailAudioPipeline,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "pipeline",
       .action = CommandAction::OpenQuality,
       .detail = i18n::MessageId::TuiShellDetailAudioPipeline,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "a",
       .action = CommandAction::OpenQuality,
       .detail = i18n::MessageId::TuiShellDetailAudioPipeline,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "output",
       .action = CommandAction::OpenOutputDevices,
       .detail = i18n::MessageId::TuiShellDetailOutputDevice,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "outputs",
       .action = CommandAction::OpenOutputDevices,
       .detail = i18n::MessageId::TuiShellDetailOutputDevice,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "device",
       .action = CommandAction::OpenOutputDevices,
       .detail = i18n::MessageId::TuiShellDetailOutputDevice,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "devices",
       .action = CommandAction::OpenOutputDevices,
       .detail = i18n::MessageId::TuiShellDetailOutputDevice,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "o",
       .action = CommandAction::OpenOutputDevices,
       .detail = i18n::MessageId::TuiShellDetailOutputDevice,
       .category = i18n::MessageId::TuiShellCategoryAudio},
      {.alias = "views",
       .action = CommandAction::OpenPresentationPanel,
       .detail = i18n::MessageId::TuiShellDetailChooseView,
       .category = i18n::MessageId::TuiShellCategoryView},
      {.alias = "v",
       .action = CommandAction::OpenPresentationPanel,
       .detail = i18n::MessageId::TuiShellDetailChooseView,
       .category = i18n::MessageId::TuiShellCategoryView},
      {.alias = "notifications",
       .action = CommandAction::OpenNotifications,
       .detail = i18n::MessageId::TuiShellDetailNotificationCenter,
       .category = i18n::MessageId::TuiShellCategoryStatus},
      {.alias = "notification",
       .action = CommandAction::OpenNotifications,
       .detail = i18n::MessageId::TuiShellDetailNotificationCenter,
       .category = i18n::MessageId::TuiShellCategoryStatus},
      {.alias = "n",
       .action = CommandAction::OpenNotifications,
       .detail = i18n::MessageId::TuiShellDetailNotificationCenter,
       .category = i18n::MessageId::TuiShellCategoryStatus},
      {.alias = "close",
       .action = CommandAction::CloseOverlay,
       .detail = i18n::MessageId::TuiShellDetailCloseOverlay,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "hide",
       .action = CommandAction::CloseOverlay,
       .detail = i18n::MessageId::TuiShellDetailCloseOverlay,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "esc",
       .action = CommandAction::CloseOverlay,
       .detail = i18n::MessageId::TuiShellDetailCloseOverlay,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "help",
       .action = CommandAction::ShowHelp,
       .detail = i18n::MessageId::TuiShellDetailHelp,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "h",
       .action = CommandAction::ShowHelp,
       .detail = i18n::MessageId::TuiShellDetailHelp,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "?",
       .action = CommandAction::ShowHelp,
       .detail = i18n::MessageId::TuiShellDetailHelp,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "current",
       .action = CommandAction::RevealCurrentTrack,
       .detail = i18n::MessageId::TuiShellDetailNowPlaying,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "now",
       .action = CommandAction::RevealCurrentTrack,
       .detail = i18n::MessageId::TuiShellDetailNowPlaying,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "reveal",
       .action = CommandAction::RevealCurrentTrack,
       .detail = i18n::MessageId::TuiShellDetailNowPlaying,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "clear",
       .action = CommandAction::ClearFilter,
       .detail = i18n::MessageId::TuiShellDetailClearFilter,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "c",
       .action = CommandAction::ClearFilter,
       .detail = i18n::MessageId::TuiShellDetailClearFilter,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "reload",
       .action = CommandAction::Reload,
       .detail = i18n::MessageId::TuiShellDetailReloadList,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "refresh",
       .action = CommandAction::Reload,
       .detail = i18n::MessageId::TuiShellDetailReloadList,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "r",
       .action = CommandAction::Reload,
       .detail = i18n::MessageId::TuiShellDetailReloadList,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "scan",
       .action = CommandAction::Scan,
       .detail = i18n::MessageId::TuiShellDetailScan,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "rescan",
       .action = CommandAction::Scan,
       .detail = i18n::MessageId::TuiShellDetailScan,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "scan cancel",
       .action = CommandAction::ScanCancel,
       .detail = i18n::MessageId::TuiShellDetailScanCancel,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "play",
       .action = CommandAction::Play,
       .detail = i18n::MessageId::TuiShellDetailPlay,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "p",
       .action = CommandAction::Play,
       .detail = i18n::MessageId::TuiShellDetailPlay,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "pause",
       .action = CommandAction::TogglePlayback,
       .detail = i18n::MessageId::TuiShellDetailPause,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "toggle",
       .action = CommandAction::TogglePlayback,
       .detail = i18n::MessageId::TuiShellDetailTogglePlayback,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "space",
       .action = CommandAction::TogglePlayback,
       .detail = i18n::MessageId::TuiShellDetailTogglePlayback,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "stop",
       .action = CommandAction::Stop,
       .detail = i18n::MessageId::TuiShellDetailStop,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "s",
       .action = CommandAction::Stop,
       .detail = i18n::MessageId::TuiShellDetailStop,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "quit",
       .action = CommandAction::Quit,
       .detail = i18n::MessageId::TuiShellDetailQuit,
       .category = i18n::MessageId::TuiShellCategoryApp},
      {.alias = "q",
       .action = CommandAction::Quit,
       .detail = i18n::MessageId::TuiShellDetailQuit,
       .category = i18n::MessageId::TuiShellCategoryApp},
    });

    struct CommandKeyAction final
    {
      CommandAction command = CommandAction::Quit;
      TuiKeyAction key = TuiKeyAction::Quit;
    };

    constexpr auto kCommandKeyActions = std::to_array<CommandKeyAction>({
      {.command = CommandAction::OpenLists, .key = TuiKeyAction::ToggleListChooser},
      {.command = CommandAction::OpenDetail, .key = TuiKeyAction::ToggleDetails},
      {.command = CommandAction::OpenQuality, .key = TuiKeyAction::ToggleAudioPipeline},
      {.command = CommandAction::OpenOutputDevices, .key = TuiKeyAction::ToggleOutputDevices},
      {.command = CommandAction::OpenPresentationPanel, .key = TuiKeyAction::TogglePresentations},
      {.command = CommandAction::OpenNotifications, .key = TuiKeyAction::ToggleNotifications},
      {.command = CommandAction::ShowHelp, .key = TuiKeyAction::ShowHelp},
      {.command = CommandAction::RevealCurrentTrack, .key = TuiKeyAction::RevealCurrentTrack},
      {.command = CommandAction::ClearFilter, .key = TuiKeyAction::ClearFilter},
      {.command = CommandAction::Reload, .key = TuiKeyAction::Reload},
      {.command = CommandAction::Scan, .key = TuiKeyAction::Scan},
      {.command = CommandAction::ScanCancel, .key = TuiKeyAction::ScanCancel},
      {.command = CommandAction::Play, .key = TuiKeyAction::PlaySelection},
      {.command = CommandAction::TogglePlayback, .key = TuiKeyAction::PlaybackPlayPause},
      {.command = CommandAction::Stop, .key = TuiKeyAction::PlaybackStop},
      {.command = CommandAction::Quit, .key = TuiKeyAction::Quit},
    });

    std::string lower(std::string value)
    {
      std::ranges::transform(value, value.begin(), utility::toAsciiLower);
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

  std::optional<TuiKeyAction> shortcutActionForCommand(CommandAction const action) noexcept
  {
    for (auto const& relation : kCommandKeyActions)
    {
      if (relation.command == action)
      {
        return relation.key;
      }
    }

    return std::nullopt;
  }

  std::optional<CommandAction> commandActionForKeyAction(TuiKeyAction const action) noexcept
  {
    for (auto const& relation : kCommandKeyActions)
    {
      if (relation.key == action)
      {
        return relation.command;
      }
    }

    return std::nullopt;
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

  std::string_view overlayLabel(i18n::MessageCatalog const& textCatalog, Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayTracks);
      case Overlay::ListChooser: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayLists);
      case Overlay::DetailPanel: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayDetail);
      case Overlay::QualityPanel: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayPipeline);
      case Overlay::OutputDevices: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayOutput);
      case Overlay::PresentationPanel: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayViews);
      case Overlay::Notifications:
        return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayNotifications);
      case Overlay::Help: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayHelp);
    }

    return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayTracks);
  }

  std::string_view overlayToggleShortcut(TuiKeymapPlan const& keymapPlan, Overlay const overlay)
  {
    static auto const selectionEvents = std::to_array({ftxui::Event::Return});
    static auto const notificationEvents = std::to_array({ftxui::Event::Character("x")});

    switch (overlay)
    {
      case Overlay::ListChooser: return keymapPlan.shortcutFor(TuiKeyAction::ToggleListChooser, selectionEvents);
      case Overlay::DetailPanel: return keymapPlan.shortcutFor(TuiKeyAction::ToggleDetails);
      case Overlay::QualityPanel: return keymapPlan.shortcutFor(TuiKeyAction::ToggleAudioPipeline);
      case Overlay::OutputDevices: return keymapPlan.shortcutFor(TuiKeyAction::ToggleOutputDevices, selectionEvents);
      case Overlay::PresentationPanel:
        return keymapPlan.shortcutFor(TuiKeyAction::TogglePresentations, selectionEvents);
      case Overlay::Notifications: return keymapPlan.shortcutFor(TuiKeyAction::ToggleNotifications, notificationEvents);
      case Overlay::None:
      case Overlay::Help: return {};
    }

    return {};
  }

  std::string overlayHint(i18n::MessageCatalog const& textCatalog,
                          TuiKeymapPlan const& keymapPlan,
                          Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return {};
      case Overlay::ListChooser:
        return tuiOverlayHint(
          textCatalog, i18n::MessageId::TuiShellHintLists, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::DetailPanel:
        return tuiOverlayHint(
          textCatalog, i18n::MessageId::TuiShellHintDetail, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::QualityPanel:
        return tuiOverlayHint(
          textCatalog, i18n::MessageId::TuiShellHintPipeline, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::OutputDevices:
        return tuiOverlayHint(
          textCatalog, i18n::MessageId::TuiShellHintOutput, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::PresentationPanel:
        return tuiOverlayHint(
          textCatalog, i18n::MessageId::TuiShellHintViews, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::Notifications:
        return tuiOverlayHint(
          textCatalog, i18n::MessageId::TuiShellHintNotifications, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::Help: return tuiOverlayHint(textCatalog, i18n::MessageId::TuiShellHintHelp, {});
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
