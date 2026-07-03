// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/detail/FieldCatalog.h>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace ao::query::detail
{
  namespace
  {
    constexpr auto kNoAliases = std::array<std::string_view, 0>{};

    constexpr auto kTitleAliases = std::to_array<std::string_view>({"t"});
    constexpr auto kArtistAliases = std::to_array<std::string_view>({"a"});
    constexpr auto kAlbumAliases = std::to_array<std::string_view>({"al"});
    constexpr auto kAlbumArtistAliases = std::to_array<std::string_view>({"aa"});
    constexpr auto kComposerAliases = std::to_array<std::string_view>({"c"});
    constexpr auto kWorkAliases = std::to_array<std::string_view>({"w"});
    constexpr auto kMovementAliases = std::to_array<std::string_view>({"m"});
    constexpr auto kGenreAliases = std::to_array<std::string_view>({"g"});
    constexpr auto kYearAliases = std::to_array<std::string_view>({"y"});
    constexpr auto kTrackNumberAliases = std::to_array<std::string_view>({"tn"});
    constexpr auto kTrackTotalAliases = std::to_array<std::string_view>({"tt"});
    constexpr auto kDiscNumberAliases = std::to_array<std::string_view>({"dn"});
    constexpr auto kDiscTotalAliases = std::to_array<std::string_view>({"td"});
    constexpr auto kMovementNumberAliases = std::to_array<std::string_view>({"mn"});
    constexpr auto kMovementTotalAliases = std::to_array<std::string_view>({"mt"});
    constexpr auto kCoverArtAliases = std::to_array<std::string_view>({"ca"});

    constexpr auto kDurationAliases = std::to_array<std::string_view>({"l"});
    constexpr auto kBitrateAliases = std::to_array<std::string_view>({"br"});
    constexpr auto kSampleRateAliases = std::to_array<std::string_view>({"sr"});
    constexpr auto kBitDepthAliases = std::to_array<std::string_view>({"bd"});

    constexpr auto kMetadataSpecs = std::to_array<QueryVariableCompletionSpec>({
      {.type = VariableType::Metadata,
       .field = Field::Title,
       .canonicalName = "title",
       .aliases = std::span{kTitleAliases}},
      {.type = VariableType::Metadata,
       .field = Field::ArtistId,
       .canonicalName = "artist",
       .aliases = std::span{kArtistAliases}},
      {.type = VariableType::Metadata,
       .field = Field::AlbumId,
       .canonicalName = "album",
       .aliases = std::span{kAlbumAliases}},
      {.type = VariableType::Metadata,
       .field = Field::AlbumArtistId,
       .canonicalName = "albumArtist",
       .aliases = std::span{kAlbumArtistAliases}},
      {.type = VariableType::Metadata,
       .field = Field::ComposerId,
       .canonicalName = "composer",
       .aliases = std::span{kComposerAliases}},
      {.type = VariableType::Metadata,
       .field = Field::WorkId,
       .canonicalName = "work",
       .aliases = std::span{kWorkAliases}},
      {.type = VariableType::Metadata,
       .field = Field::MovementId,
       .canonicalName = "movement",
       .aliases = std::span{kMovementAliases}},
      {.type = VariableType::Metadata,
       .field = Field::GenreId,
       .canonicalName = "genre",
       .aliases = std::span{kGenreAliases}},
      {.type = VariableType::Metadata,
       .field = Field::Year,
       .canonicalName = "year",
       .aliases = std::span{kYearAliases}},
      {.type = VariableType::Metadata,
       .field = Field::TrackNumber,
       .canonicalName = "trackNumber",
       .aliases = std::span{kTrackNumberAliases}},
      {.type = VariableType::Metadata,
       .field = Field::TrackTotal,
       .canonicalName = "trackTotal",
       .aliases = std::span{kTrackTotalAliases}},
      {.type = VariableType::Metadata,
       .field = Field::DiscNumber,
       .canonicalName = "discNumber",
       .aliases = std::span{kDiscNumberAliases}},
      {.type = VariableType::Metadata,
       .field = Field::DiscTotal,
       .canonicalName = "discTotal",
       .aliases = std::span{kDiscTotalAliases}},
      {.type = VariableType::Metadata,
       .field = Field::MovementNumber,
       .canonicalName = "movementNumber",
       .aliases = std::span{kMovementNumberAliases}},
      {.type = VariableType::Metadata,
       .field = Field::MovementTotal,
       .canonicalName = "movementTotal",
       .aliases = std::span{kMovementTotalAliases}},
      {.type = VariableType::Metadata,
       .field = Field::CoverArtId,
       .canonicalName = "coverArt",
       .aliases = std::span{kCoverArtAliases}},
    });

    constexpr auto kPropertySpecs = std::to_array<QueryVariableCompletionSpec>({
      {.type = VariableType::Property,
       .field = Field::Duration,
       .canonicalName = "duration",
       .aliases = std::span{kDurationAliases}},
      {.type = VariableType::Property,
       .field = Field::Bitrate,
       .canonicalName = "bitrate",
       .aliases = std::span{kBitrateAliases}},
      {.type = VariableType::Property,
       .field = Field::SampleRate,
       .canonicalName = "sampleRate",
       .aliases = std::span{kSampleRateAliases}},
      {.type = VariableType::Property,
       .field = Field::Channels,
       .canonicalName = "channels",
       .aliases = std::span{kNoAliases}},
      {.type = VariableType::Property,
       .field = Field::BitDepth,
       .canonicalName = "bitDepth",
       .aliases = std::span{kBitDepthAliases}},
      {.type = VariableType::Property,
       .field = Field::Codec,
       .canonicalName = "codec",
       .aliases = std::span{kNoAliases}},
    });

    constexpr auto kEmptySpecs = std::array<QueryVariableCompletionSpec, 0>{};

    bool matchesCatalogName(QueryVariableCompletionSpec const& spec, std::string_view name)
    {
      return spec.canonicalName == name ||
             std::ranges::any_of(spec.aliases, [name](std::string_view alias) { return alias == name; });
    }
  } // namespace

  std::span<QueryVariableCompletionSpec const> queryVariableCompletionSpecs(VariableType type)
  {
    switch (type)
    {
      case VariableType::Metadata: return std::span{kMetadataSpecs};
      case VariableType::Property: return std::span{kPropertySpecs};
      case VariableType::Tag:
      case VariableType::Custom: return std::span{kEmptySpecs};
    }

    return std::span{kEmptySpecs};
  }

  QueryVariableCompletionSpec const* findQueryVariableCompletionSpec(VariableType type, std::string_view name)
  {
    auto const specs = queryVariableCompletionSpecs(type);
    auto const iter = std::ranges::find_if(
      specs, [name](QueryVariableCompletionSpec const& spec) { return matchesCatalogName(spec, name); });

    if (iter == specs.end())
    {
      return nullptr;
    }

    return &*iter;
  }
} // namespace ao::query::detail
