// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TuiTextCatalog.h"

#include <ao/i18n/MessageCatalog.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui
{
  using i18n::MessageArgument;
  using i18n::MessageCatalog;
  using i18n::MessageId;
  using i18n::requiredFormat;
  using i18n::requiredText;

  namespace
  {
    std::string commandHelp(MessageCatalog const& catalog, MessageId const id, std::string_view const command)
    {
      return requiredFormat(catalog, id, {MessageArgument{"command", command}});
    }

    std::string commandAliasHelp(MessageCatalog const& catalog,
                                 MessageId const id,
                                 std::string_view const command,
                                 std::string_view const alias)
    {
      return requiredFormat(catalog, id, {MessageArgument{"command", command}, MessageArgument{"alias", alias}});
    }
  } // namespace

  std::string tuiChromeText(MessageCatalog const& catalog, MessageId const id)
  {
    switch (id)
    {
      case MessageId::TuiShellCommandPaletteFooter:
        return requiredFormat(catalog,
                              id,
                              {MessageArgument{"completeKey", "Tab"},
                               MessageArgument{"runKey", "Enter"},
                               MessageArgument{"cancelKey", "Esc"}});
      case MessageId::TuiShellQuickFilterFooter:
        return requiredFormat(catalog,
                              id,
                              {MessageArgument{"acceptKey", "Enter"},
                               MessageArgument{"completeKey", "Tab"},
                               MessageArgument{"keepKey", "Esc"}});
      case MessageId::TuiShellHintLists:
        return requiredFormat(
          catalog,
          id,
          {MessageArgument{"toggleKey", "l"}, MessageArgument{"openKey", "Enter"}, MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintDetail:
        return requiredFormat(catalog, id, {MessageArgument{"toggleKey", "d"}, MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintPipeline:
        return requiredFormat(catalog, id, {MessageArgument{"toggleKey", "a"}, MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintOutput:
        return requiredFormat(catalog,
                              id,
                              {MessageArgument{"toggleKey", "o"},
                               MessageArgument{"selectKey", "Enter"},
                               MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintViews:
        return requiredFormat(catalog,
                              id,
                              {MessageArgument{"toggleKey", "v"},
                               MessageArgument{"selectKey", "Enter"},
                               MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintNotifications:
        return requiredFormat(
          catalog,
          id,
          {MessageArgument{"toggleKey", "n"}, MessageArgument{"hideKey", "x"}, MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintHelp: return requiredFormat(catalog, id, {MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHelpQuickFilter: return commandHelp(catalog, id, "/text");
      case MessageId::TuiShellHelpChooseList: return commandAliasHelp(catalog, id, ":lists", ":l");
      case MessageId::TuiShellHelpTrackDetail: return commandAliasHelp(catalog, id, ":detail", ":d");
      case MessageId::TuiShellHelpAudioPipeline: return commandAliasHelp(catalog, id, ":pipeline", ":a");
      case MessageId::TuiShellHelpOutputDevice: return commandAliasHelp(catalog, id, ":output", ":o");
      case MessageId::TuiShellHelpChooseView: return commandAliasHelp(catalog, id, ":views", ":v");
      case MessageId::TuiShellHelpNotifications: return commandAliasHelp(catalog, id, ":notifications", ":n");
      case MessageId::TuiShellHelpCurrentTrack: return commandHelp(catalog, id, ":current");
      case MessageId::TuiShellHelpSwitchPresentation: return commandHelp(catalog, id, ":view <id>");
      case MessageId::TuiShellHelpPreviousNextGroup: return commandHelp(catalog, id, "{ / }");
      case MessageId::TuiShellHelpClearFilter: return commandHelp(catalog, id, ":clear");
      case MessageId::TuiShellHelpReloadList: return commandHelp(catalog, id, ":reload");
      case MessageId::TuiShellHelpPlayback: return commandHelp(catalog, id, ":play :pause :stop");
      case MessageId::TuiShellHelpQuit: return commandHelp(catalog, id, ":quit");
      case MessageId::TuiShellHelpFooter:
        return requiredFormat(catalog, id, {MessageArgument{"closeKey", "Esc"}, MessageArgument{"runKey", "Enter"}});
      case MessageId::TuiShellNotificationFooter:
        return requiredFormat(
          catalog,
          id,
          {MessageArgument{"toggleKey", "n"}, MessageArgument{"hideKey", "x"}, MessageArgument{"closeKey", "Esc"}});
      default: return std::string{requiredText(catalog, id)};
    }
  }

  std::string playbackVolume(MessageCatalog const& catalog, std::int32_t const percent)
  {
    return requiredFormat(catalog, MessageId::TuiPlaybackVolume, {MessageArgument{"percent", percent}});
  }

  std::string librarySection(MessageCatalog const& catalog, std::string_view const name)
  {
    return requiredFormat(catalog, MessageId::TuiLibrarySection, {MessageArgument{"name", name}});
  }

  std::string libraryRevealedTrack(MessageCatalog const& catalog, std::string_view const track)
  {
    return requiredFormat(catalog, MessageId::TuiLibraryRevealedTrack, {MessageArgument{"track", track}});
  }

  std::string libraryUnknownView(MessageCatalog const& catalog, std::string_view const id)
  {
    return requiredFormat(catalog, MessageId::TuiLibraryUnknownView, {MessageArgument{"id", id}});
  }

  std::string libraryView(MessageCatalog const& catalog, std::string_view const id)
  {
    return requiredFormat(catalog, MessageId::TuiLibraryView, {MessageArgument{"id", id}});
  }

  std::string libraryOpenedList(MessageCatalog const& catalog, std::string_view const list)
  {
    return requiredFormat(catalog, MessageId::TuiLibraryOpenedList, {MessageArgument{"list", list}});
  }

  std::string libraryReloadedTracks(MessageCatalog const& catalog, std::size_t const count)
  {
    return requiredFormat(catalog, MessageId::TuiLibraryReloadedTracks, {MessageArgument{"count", count}});
  }

  std::string libraryQuickFilterMatched(MessageCatalog const& catalog, std::size_t const count)
  {
    return requiredFormat(catalog, MessageId::TuiLibraryQuickFilterMatched, {MessageArgument{"count", count}});
  }

  std::string libraryExpressionFilterMatched(MessageCatalog const& catalog, std::size_t const count)
  {
    return requiredFormat(catalog, MessageId::TuiLibraryExpressionFilterMatched, {MessageArgument{"count", count}});
  }
} // namespace ao::tui
