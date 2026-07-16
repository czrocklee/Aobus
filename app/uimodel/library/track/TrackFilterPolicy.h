// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::detail
{
  inline constexpr auto kQuickFilterFields = std::to_array<rt::TrackField>({
    rt::TrackField::Title,
    rt::TrackField::Artist,
    rt::TrackField::Album,
    rt::TrackField::AlbumArtist,
    rt::TrackField::Genre,
    rt::TrackField::Composer,
    rt::TrackField::Work,
  });

  struct QuickFilterCompletionToken final
  {
    std::size_t replaceBegin = 0;
    std::size_t replaceEnd = 0;
    std::string prefix;
  };

  bool isExplicitTrackFilterExpression(std::string_view text);
  std::vector<std::string> splitQuickFilterTerms(std::string_view text);
  std::optional<QuickFilterCompletionToken> analyzeQuickFilterCompletion(std::string_view text, std::size_t cursor);
} // namespace ao::uimodel::detail
