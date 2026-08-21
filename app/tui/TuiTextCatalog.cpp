// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TuiTextCatalog.h"

#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageArgument;
    using i18n::MessageId;

    constexpr auto kMessageIds = std::to_array<MessageId>({
      MessageId::TuiShellCommandPaletteTitle,
      MessageId::TuiShellCommandPaletteNoMatches,
      MessageId::TuiShellCommandPaletteFooter,
      MessageId::TuiShellQuickFilterTitle,
      MessageId::TuiShellQuickFilterFooter,
      MessageId::TuiShellWorkspaceList,
      MessageId::TuiShellWorkspaceView,
      MessageId::TuiShellOverlayTracks,
      MessageId::TuiShellOverlayLists,
      MessageId::TuiShellOverlayDetail,
      MessageId::TuiShellOverlayPipeline,
      MessageId::TuiShellOverlayOutput,
      MessageId::TuiShellOverlayViews,
      MessageId::TuiShellOverlayNotifications,
      MessageId::TuiShellOverlayHelp,
      MessageId::TuiShellHintLists,
      MessageId::TuiShellHintDetail,
      MessageId::TuiShellHintPipeline,
      MessageId::TuiShellHintOutput,
      MessageId::TuiShellHintViews,
      MessageId::TuiShellHintNotifications,
      MessageId::TuiShellHintHelp,
      MessageId::TuiShellStatusCommand,
      MessageId::TuiShellStatusLists,
      MessageId::TuiShellStatusView,
      MessageId::TuiShellStatusDetail,
      MessageId::TuiShellStatusClearFilter,
      MessageId::TuiShellStatusHelp,
      MessageId::TuiShellFilterLabel,
      MessageId::TuiShellCategoryLibrary,
      MessageId::TuiShellCategoryView,
      MessageId::TuiShellCategoryTrack,
      MessageId::TuiShellCategoryAudio,
      MessageId::TuiShellCategoryStatus,
      MessageId::TuiShellCategoryUi,
      MessageId::TuiShellCategoryPlayback,
      MessageId::TuiShellCategoryApp,
      MessageId::TuiShellDetailQuickFilter,
      MessageId::TuiShellDetailTrackView,
      MessageId::TuiShellDetailChooseList,
      MessageId::TuiShellDetailTrackDetail,
      MessageId::TuiShellDetailAudioPipeline,
      MessageId::TuiShellDetailOutputDevice,
      MessageId::TuiShellDetailChooseView,
      MessageId::TuiShellDetailNotificationCenter,
      MessageId::TuiShellDetailCloseOverlay,
      MessageId::TuiShellDetailHelp,
      MessageId::TuiShellDetailNowPlaying,
      MessageId::TuiShellDetailClearFilter,
      MessageId::TuiShellDetailReloadList,
      MessageId::TuiShellDetailPlay,
      MessageId::TuiShellDetailPause,
      MessageId::TuiShellDetailTogglePlayback,
      MessageId::TuiShellDetailStop,
      MessageId::TuiShellDetailQuit,
      MessageId::TuiShellOutputDevicesTitle,
      MessageId::TuiPlaybackNoActiveTrack,
      MessageId::TuiPlaybackNoOutputDeviceSelected,
      MessageId::TuiPlaybackNoOutputDevicesFound,
      MessageId::TuiPlaybackNoAudioPipeline,
      MessageId::TuiShellHelpQuickFilter,
      MessageId::TuiShellHelpChooseList,
      MessageId::TuiShellHelpTrackDetail,
      MessageId::TuiShellHelpAudioPipeline,
      MessageId::TuiShellHelpOutputDevice,
      MessageId::TuiShellHelpChooseView,
      MessageId::TuiShellHelpNotifications,
      MessageId::TuiShellHelpCurrentTrack,
      MessageId::TuiShellHelpSwitchPresentation,
      MessageId::TuiShellHelpPreviousNextGroup,
      MessageId::TuiShellHelpClearFilter,
      MessageId::TuiShellHelpReloadList,
      MessageId::TuiShellHelpPlayback,
      MessageId::TuiShellHelpQuit,
      MessageId::TuiShellHelpFooter,
      MessageId::TuiShellNotificationFooter,
      MessageId::TuiLibraryDetail,
      MessageId::TuiLibraryNoSections,
      MessageId::TuiLibraryNoSectionSelected,
      MessageId::TuiLibraryNoCurrentTrack,
      MessageId::TuiLibraryCurrentTrackNotInView,
      MessageId::TuiLibraryNoActiveTrackView,
      MessageId::TuiLibraryNoViewsAvailable,
      MessageId::TuiLibraryNoListsAvailable,
      MessageId::TuiLibraryFilterCleared,
      MessageId::TuiLibraryFilterApplied,
      MessageId::TuiLibraryNoTracksFound,
      MessageId::TuiLibraryNoListsFound,
      MessageId::TuiLibraryReady,
    });

    static_assert(kMessageIds.size() == static_cast<std::size_t>(TuiTextId::Count));

    std::string requiredMessage(i18n::MessageCatalog const& catalog,
                                i18n::MessageId const id,
                                std::initializer_list<i18n::MessageArgument> const arguments = {})
    {
      auto result = catalog.format(id, arguments);

      if (!result)
      {
        AO_FATAL("Could not format required TUI message: {}", result.error().message);
      }

      return std::move(result->text);
    }

    std::string commandHelp(i18n::MessageCatalog const& catalog,
                            i18n::MessageId const id,
                            std::string_view const command)
    {
      return requiredMessage(catalog, id, {MessageArgument{"command", command}});
    }

    std::string commandAliasHelp(i18n::MessageCatalog const& catalog,
                                 i18n::MessageId const id,
                                 std::string_view const command,
                                 std::string_view const alias)
    {
      return requiredMessage(catalog, id, {MessageArgument{"command", command}, MessageArgument{"alias", alias}});
    }

    std::string resolveMessage(i18n::MessageCatalog const& catalog,
                               TuiTextId const textId,
                               i18n::MessageId const messageId)
    {
      switch (textId)
      {
        case TuiTextId::CommandPaletteFooter:
          return requiredMessage(catalog,
                                 messageId,
                                 {MessageArgument{"completeKey", "Tab"},
                                  MessageArgument{"runKey", "Enter"},
                                  MessageArgument{"cancelKey", "Esc"}});
        case TuiTextId::QuickFilterFooter:
          return requiredMessage(catalog,
                                 messageId,
                                 {MessageArgument{"acceptKey", "Enter"},
                                  MessageArgument{"completeKey", "Tab"},
                                  MessageArgument{"keepKey", "Esc"}});
        case TuiTextId::HintLists:
          return requiredMessage(catalog,
                                 messageId,
                                 {MessageArgument{"toggleKey", "l"},
                                  MessageArgument{"openKey", "Enter"},
                                  MessageArgument{"closeKey", "Esc"}});
        case TuiTextId::HintDetail:
          return requiredMessage(
            catalog, messageId, {MessageArgument{"toggleKey", "d"}, MessageArgument{"closeKey", "Esc"}});
        case TuiTextId::HintPipeline:
          return requiredMessage(
            catalog, messageId, {MessageArgument{"toggleKey", "a"}, MessageArgument{"closeKey", "Esc"}});
        case TuiTextId::HintOutput:
          return requiredMessage(catalog,
                                 messageId,
                                 {MessageArgument{"toggleKey", "o"},
                                  MessageArgument{"selectKey", "Enter"},
                                  MessageArgument{"closeKey", "Esc"}});
        case TuiTextId::HintViews:
          return requiredMessage(catalog,
                                 messageId,
                                 {MessageArgument{"toggleKey", "v"},
                                  MessageArgument{"selectKey", "Enter"},
                                  MessageArgument{"closeKey", "Esc"}});
        case TuiTextId::HintNotifications:
          return requiredMessage(
            catalog,
            messageId,
            {MessageArgument{"toggleKey", "n"}, MessageArgument{"hideKey", "x"}, MessageArgument{"closeKey", "Esc"}});
        case TuiTextId::HintHelp: return requiredMessage(catalog, messageId, {MessageArgument{"closeKey", "Esc"}});
        case TuiTextId::HelpQuickFilter: return commandHelp(catalog, messageId, "/text");
        case TuiTextId::HelpChooseList: return commandAliasHelp(catalog, messageId, ":lists", ":l");
        case TuiTextId::HelpTrackDetail: return commandAliasHelp(catalog, messageId, ":detail", ":d");
        case TuiTextId::HelpAudioPipeline: return commandAliasHelp(catalog, messageId, ":pipeline", ":a");
        case TuiTextId::HelpOutputDevice: return commandAliasHelp(catalog, messageId, ":output", ":o");
        case TuiTextId::HelpChooseView: return commandAliasHelp(catalog, messageId, ":views", ":v");
        case TuiTextId::HelpNotifications: return commandAliasHelp(catalog, messageId, ":notifications", ":n");
        case TuiTextId::HelpCurrentTrack: return commandHelp(catalog, messageId, ":current");
        case TuiTextId::HelpSwitchPresentation: return commandHelp(catalog, messageId, ":view <id>");
        case TuiTextId::HelpPreviousNextGroup: return commandHelp(catalog, messageId, "{ / }");
        case TuiTextId::HelpClearFilter: return commandHelp(catalog, messageId, ":clear");
        case TuiTextId::HelpReloadList: return commandHelp(catalog, messageId, ":reload");
        case TuiTextId::HelpPlayback: return commandHelp(catalog, messageId, ":play :pause :stop");
        case TuiTextId::HelpQuit: return commandHelp(catalog, messageId, ":quit");
        case TuiTextId::HelpFooter:
          return requiredMessage(
            catalog, messageId, {MessageArgument{"closeKey", "Esc"}, MessageArgument{"runKey", "Enter"}});
        case TuiTextId::NotificationFooter:
          return requiredMessage(
            catalog,
            messageId,
            {MessageArgument{"toggleKey", "n"}, MessageArgument{"hideKey", "x"}, MessageArgument{"closeKey", "Esc"}});
        default: return requiredMessage(catalog, messageId);
      }
    }
  } // namespace

  TuiTextCatalog::TuiTextCatalog(i18n::MessageCatalog const& catalog)
    : _catalog{catalog}
  {
    for (std::size_t index = 0; index < _text.size(); ++index)
    {
      _text[index] = resolveMessage(catalog, static_cast<TuiTextId>(index), kMessageIds[index]);
    }
  }

  std::string_view TuiTextCatalog::text(TuiTextId const id) const noexcept
  {
    auto const index = static_cast<std::size_t>(id);
    return index < _text.size() ? _text[index] : std::string_view{};
  }

  std::string TuiTextCatalog::playbackVolume(std::int32_t const percent) const
  {
    auto const argument = i18n::MessageArgument{"percent", percent};
    return requiredMessage(_catalog, i18n::MessageId::TuiPlaybackVolume, {argument});
  }

  std::string TuiTextCatalog::librarySection(std::string_view const name) const
  {
    return requiredMessage(_catalog, i18n::MessageId::TuiLibrarySection, {MessageArgument{"name", name}});
  }

  std::string TuiTextCatalog::libraryRevealedTrack(std::string_view const track) const
  {
    return requiredMessage(_catalog, i18n::MessageId::TuiLibraryRevealedTrack, {MessageArgument{"track", track}});
  }

  std::string TuiTextCatalog::libraryUnknownView(std::string_view const id) const
  {
    return requiredMessage(_catalog, i18n::MessageId::TuiLibraryUnknownView, {MessageArgument{"id", id}});
  }

  std::string TuiTextCatalog::libraryView(std::string_view const id) const
  {
    return requiredMessage(_catalog, i18n::MessageId::TuiLibraryView, {MessageArgument{"id", id}});
  }

  std::string TuiTextCatalog::libraryOpenedList(std::string_view const list) const
  {
    return requiredMessage(_catalog, i18n::MessageId::TuiLibraryOpenedList, {MessageArgument{"list", list}});
  }

  std::string TuiTextCatalog::libraryReloadedTracks(std::size_t const count) const
  {
    auto const argument = i18n::MessageArgument{"count", count};
    return requiredMessage(_catalog, i18n::MessageId::TuiLibraryReloadedTracks, {argument});
  }

  std::string TuiTextCatalog::libraryQuickFilterMatched(std::size_t const count) const
  {
    auto const argument = i18n::MessageArgument{"count", count};
    return requiredMessage(_catalog, i18n::MessageId::TuiLibraryQuickFilterMatched, {argument});
  }

  std::string TuiTextCatalog::libraryExpressionFilterMatched(std::size_t const count) const
  {
    auto const argument = i18n::MessageArgument{"count", count};
    return requiredMessage(_catalog, i18n::MessageId::TuiLibraryExpressionFilterMatched, {argument});
  }
} // namespace ao::tui
