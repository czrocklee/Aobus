// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Type.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class TrackGroupBy : std::uint8_t
{
  None,
  Artist,
  Album,
  AlbumArtist,
  Genre,
  Year,
};

enum class TrackSortField : std::uint8_t
{
  Artist,
  Album,
  AlbumArtist,
  Genre,
  Year,
  DiscNumber,
  TrackNumber,
  Title,
};

enum class TrackColumn : std::uint8_t
{
  Artist,
  Album,
  DiscNumber,
  TrackNumber,
  Title,
  Tags,
};

struct TrackSortTerm
{
  TrackSortField field;

  auto operator==(TrackSortTerm const&) const -> bool = default;
};

struct TrackPresentationSpec
{
  TrackGroupBy groupBy = TrackGroupBy::None;
  std::vector<TrackSortTerm> sortBy;
};

struct TrackPresentationKeysView
{
  std::string_view artist;
  std::string_view album;
  std::string_view albumArtist;
  std::string_view genre;
  std::string_view title;
  std::uint16_t year = 0;
  std::uint16_t discNumber = 0;
  std::uint16_t trackNumber = 0;
  rs::core::TrackId trackId{};
};

[[nodiscard]] TrackPresentationSpec presentationSpecForGroup(TrackGroupBy groupBy);

[[nodiscard]] int compareForSort(TrackPresentationKeysView lhs,
                                 TrackPresentationKeysView rhs,
                                 std::span<TrackSortTerm const> sortBy);

[[nodiscard]] int compareForGrouping(TrackPresentationKeysView lhs,
                                     TrackPresentationKeysView rhs,
                                     TrackGroupBy groupBy);

[[nodiscard]] bool shouldShowColumn(TrackGroupBy groupBy, TrackColumn column);

[[nodiscard]] std::string groupLabelFor(TrackPresentationKeysView keys, TrackGroupBy groupBy);
