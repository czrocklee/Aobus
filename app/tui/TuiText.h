// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui
{
  /// Resolves argument-free text and the fixed argument-bearing TUI chrome cases; other invalid selections fail closed.
  std::string tuiChromeText(i18n::MessageCatalog const& catalog, i18n::MessageId id);
  std::string playbackVolume(i18n::MessageCatalog const& catalog, std::int32_t percent);
  std::string librarySection(i18n::MessageCatalog const& catalog, std::string_view name);
  std::string libraryRevealedTrack(i18n::MessageCatalog const& catalog, std::string_view track);
  std::string libraryUnknownView(i18n::MessageCatalog const& catalog, std::string_view id);
  std::string libraryView(i18n::MessageCatalog const& catalog, std::string_view id);
  std::string libraryOpenedList(i18n::MessageCatalog const& catalog, std::string_view list);
  std::string libraryReloadedTracks(i18n::MessageCatalog const& catalog, std::size_t count);
  std::string libraryQuickFilterMatched(i18n::MessageCatalog const& catalog, std::size_t count);
  std::string libraryExpressionFilterMatched(i18n::MessageCatalog const& catalog, std::size_t count);
} // namespace ao::tui
