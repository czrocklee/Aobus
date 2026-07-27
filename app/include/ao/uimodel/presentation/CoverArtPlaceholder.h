// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  enum class CoverArtPlaceholderStyle : std::uint8_t
  {
    Monogram,
    Note,
    Vinyl,
    Equalizer,
    Soul,
  };

  enum class CoverArtPlaceholderSlot : std::uint8_t
  {
    GroupHeading,
    Inspector,
    NowPlaying,
  };

  enum class CoverArtPlaceholderMonogramSize : std::uint8_t
  {
    Regular,
    Compact,
  };

  struct CoverArtPlaceholderRgb final
  {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    bool operator==(CoverArtPlaceholderRgb const&) const = default;
  };

  struct CoverArtPlaceholderIdentity final
  {
    std::string primaryText{};
    std::optional<std::string> optMonogram{};

    bool operator==(CoverArtPlaceholderIdentity const&) const = default;
  };

  struct CoverArtPlaceholderPresentation final
  {
    CoverArtPlaceholderStyle style = CoverArtPlaceholderStyle::Note;
    std::string monogram{"?"};
    CoverArtPlaceholderMonogramSize monogramSize = CoverArtPlaceholderMonogramSize::Regular;
    CoverArtPlaceholderRgb monogramColor{};

    bool operator==(CoverArtPlaceholderPresentation const&) const = default;
  };

  struct CoverArtPlaceholderStyleEntry final
  {
    CoverArtPlaceholderStyle style = CoverArtPlaceholderStyle::Note;
    std::string_view id{};

    bool operator==(CoverArtPlaceholderStyleEntry const&) const = default;
  };

  inline constexpr auto kCoverArtPlaceholderStyles = std::to_array<CoverArtPlaceholderStyleEntry>({
    {.style = CoverArtPlaceholderStyle::Monogram, .id = "monogram"},
    {.style = CoverArtPlaceholderStyle::Note, .id = "note"},
    {.style = CoverArtPlaceholderStyle::Vinyl, .id = "vinyl"},
    {.style = CoverArtPlaceholderStyle::Equalizer, .id = "equalizer"},
    {.style = CoverArtPlaceholderStyle::Soul, .id = "soul"},
  });

  std::string_view coverArtPlaceholderStyleId(CoverArtPlaceholderStyle style) noexcept;
  std::optional<CoverArtPlaceholderStyle> parseCoverArtPlaceholderStyle(std::string_view id) noexcept;
  std::vector<std::string> coverArtPlaceholderStyleIds();
  CoverArtPlaceholderStyle defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot slot) noexcept;

  CoverArtPlaceholderIdentity makeCoverArtPlaceholderIdentity(std::span<std::string_view const> candidateTexts);
  CoverArtPlaceholderPresentation makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle style,
                                                                      CoverArtPlaceholderIdentity const& identity);
} // namespace ao::uimodel
