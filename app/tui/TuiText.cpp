// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TuiText.h"

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
      case MessageId::TuiShellHelpFooter: return requiredFormat(catalog, id, {MessageArgument{"closeKey", "Esc"}});
      default: return std::string{requiredText(catalog, id)};
    }
  }

  std::string tuiNotificationFooter(MessageCatalog const& catalog, std::string_view const toggleKey)
  {
    auto const toggleState = toggleKey.empty() ? std::string_view{"unbound"} : std::string_view{"bound"};
    return requiredFormat(catalog,
                          MessageId::TuiShellNotificationFooter,
                          {MessageArgument{"toggleState", toggleState},
                           MessageArgument{"toggleKey", toggleKey},
                           MessageArgument{"hideKey", "x"},
                           MessageArgument{"closeKey", "Esc"}});
  }

  std::string tuiOverlayHint(MessageCatalog const& catalog, MessageId const id, std::string_view const toggleKey)
  {
    auto const toggleState = toggleKey.empty() ? std::string_view{"unbound"} : std::string_view{"bound"};

    switch (id)
    {
      case MessageId::TuiShellHintLists:
        return requiredFormat(catalog,
                              id,
                              {MessageArgument{"toggleState", toggleState},
                               MessageArgument{"toggleKey", toggleKey},
                               MessageArgument{"openKey", "Enter"},
                               MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintDetail:
      case MessageId::TuiShellHintPipeline:
        return requiredFormat(catalog,
                              id,
                              {MessageArgument{"toggleState", toggleState},
                               MessageArgument{"toggleKey", toggleKey},
                               MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintOutput:
      case MessageId::TuiShellHintViews:
        return requiredFormat(catalog,
                              id,
                              {MessageArgument{"toggleState", toggleState},
                               MessageArgument{"toggleKey", toggleKey},
                               MessageArgument{"selectKey", "Enter"},
                               MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintNotifications:
        return requiredFormat(catalog,
                              id,
                              {MessageArgument{"toggleState", toggleState},
                               MessageArgument{"toggleKey", toggleKey},
                               MessageArgument{"hideKey", "x"},
                               MessageArgument{"closeKey", "Esc"}});
      case MessageId::TuiShellHintHelp: return requiredFormat(catalog, id, {MessageArgument{"closeKey", "Esc"}});
      default: return {};
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
