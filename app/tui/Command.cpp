// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Command.h"

#include "Keymap.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/utility/String.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::tui
{
  namespace
  {
    constexpr auto kPrefixCommands = std::to_array<CommandPrefixSpec>({
      {.prefix = "filter ",
       .action = CommandAction::QuickFilter,
       .detail = i18n::MessageId::TuiShellDetailQuickFilter,
       .category = i18n::MessageId::TuiShellCategoryLibrary,
       .optShortcutAction = KeyAction::OpenQuickFilter},
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
       .optShortcutAction = KeyAction::TogglePresentations},
    });

    constexpr auto kAliasCommands = std::to_array<CommandAliasSpec>({
      {.alias = "lists",
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
      {.alias = "views",
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
      {.alias = "reload",
       .action = CommandAction::Reload,
       .detail = i18n::MessageId::TuiShellDetailReloadList,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "refresh",
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
      {.alias = "select toggle",
       .action = CommandAction::SelectToggle,
       .detail = i18n::MessageId::TuiShellDetailSelectToggle,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "select visual",
       .action = CommandAction::SelectVisual,
       .detail = i18n::MessageId::TuiShellDetailSelectVisual,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "select all",
       .action = CommandAction::SelectAll,
       .detail = i18n::MessageId::TuiShellDetailSelectAll,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "select clear",
       .action = CommandAction::SelectClear,
       .detail = i18n::MessageId::TuiShellDetailSelectClear,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "settings",
       .action = CommandAction::OpenSettings,
       .detail = i18n::MessageId::TuiSettingsTitle,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "config",
       .action = CommandAction::OpenSettings,
       .detail = i18n::MessageId::TuiSettingsTitle,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "edit",
       .action = CommandAction::EditProperties,
       .detail = i18n::MessageId::TuiShellDetailEditProperties,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "properties",
       .action = CommandAction::EditProperties,
       .detail = i18n::MessageId::TuiShellDetailEditProperties,
       .category = i18n::MessageId::TuiShellCategoryLibrary},
      {.alias = "play",
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
      {.alias = "previous",
       .action = CommandAction::Previous,
       .detail = i18n::MessageId::PlaybackControlPreviousTrack,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "next",
       .action = CommandAction::Next,
       .detail = i18n::MessageId::PlaybackControlNextTrack,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "shuffle",
       .action = CommandAction::Shuffle,
       .detail = i18n::MessageId::PlaybackActionToggleShuffle,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "repeat",
       .action = CommandAction::Repeat,
       .detail = i18n::MessageId::PlaybackActionCycleRepeat,
       .category = i18n::MessageId::TuiShellCategoryPlayback},
      {.alias = "back",
       .action = CommandAction::Back,
       .detail = i18n::MessageId::TuiWorkspaceBack,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "forward",
       .action = CommandAction::Forward,
       .detail = i18n::MessageId::TuiWorkspaceForward,
       .category = i18n::MessageId::TuiShellCategoryUi},
      {.alias = "quit",
       .action = CommandAction::Quit,
       .detail = i18n::MessageId::TuiShellDetailQuit,
       .category = i18n::MessageId::TuiShellCategoryApp},
    });

    struct CommandKeyAction final
    {
      CommandAction command = CommandAction::Quit;
      KeyAction key = KeyAction::Quit;
    };

    constexpr auto kCommandKeyActions = std::to_array<CommandKeyAction>({
      {.command = CommandAction::OpenLists, .key = KeyAction::ToggleLists},
      {.command = CommandAction::OpenDetail, .key = KeyAction::ToggleDetails},
      {.command = CommandAction::OpenQuality, .key = KeyAction::ToggleAudioPipeline},
      {.command = CommandAction::OpenOutputDevices, .key = KeyAction::ToggleOutputDevices},
      {.command = CommandAction::OpenPresentationPanel, .key = KeyAction::TogglePresentations},
      {.command = CommandAction::OpenNotifications, .key = KeyAction::ToggleNotifications},
      {.command = CommandAction::ShowHelp, .key = KeyAction::ShowHelp},
      {.command = CommandAction::RevealCurrentTrack, .key = KeyAction::RevealCurrentTrack},
      {.command = CommandAction::ClearFilter, .key = KeyAction::ClearFilter},
      {.command = CommandAction::Reload, .key = KeyAction::Reload},
      {.command = CommandAction::Scan, .key = KeyAction::Scan},
      {.command = CommandAction::ScanCancel, .key = KeyAction::ScanCancel},
      {.command = CommandAction::SelectToggle, .key = KeyAction::SelectToggle},
      {.command = CommandAction::SelectVisual, .key = KeyAction::SelectVisual},
      {.command = CommandAction::SelectAll, .key = KeyAction::SelectAll},
      {.command = CommandAction::SelectClear, .key = KeyAction::SelectClear},
      {.command = CommandAction::OpenSettings, .key = KeyAction::OpenSettings},
      {.command = CommandAction::EditProperties, .key = KeyAction::EditProperties},
      {.command = CommandAction::Play, .key = KeyAction::PlaySelection},
      {.command = CommandAction::TogglePlayback, .key = KeyAction::PlaybackPlayPause},
      {.command = CommandAction::Stop, .key = KeyAction::PlaybackStop},
      {.command = CommandAction::Previous, .key = KeyAction::PlaybackPrevious},
      {.command = CommandAction::Next, .key = KeyAction::PlaybackNext},
      {.command = CommandAction::Shuffle, .key = KeyAction::PlaybackShuffle},
      {.command = CommandAction::Repeat, .key = KeyAction::PlaybackRepeat},
      {.command = CommandAction::Quit, .key = KeyAction::Quit},
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

  std::optional<KeyAction> shortcutActionForCommand(CommandAction const action) noexcept
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

  std::optional<CommandAction> commandActionForKeyAction(KeyAction const action) noexcept
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
} // namespace ao::tui
