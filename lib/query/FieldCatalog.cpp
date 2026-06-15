// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace ao::query
{
  namespace
  {
    constexpr auto kNoAliases = std::array<std::string_view, 0>{};

    constexpr auto kTitleAliases = std::array{std::string_view{"t"}};
    constexpr auto kArtistAliases = std::array{std::string_view{"a"}};
    constexpr auto kAlbumAliases = std::array{std::string_view{"al"}};
    constexpr auto kAlbumArtistAliases = std::array{std::string_view{"aa"}};
    constexpr auto kComposerAliases = std::array{std::string_view{"c"}};
    constexpr auto kWorkAliases = std::array{std::string_view{"w"}};
    constexpr auto kGenreAliases = std::array{std::string_view{"g"}};
    constexpr auto kYearAliases = std::array{std::string_view{"y"}};
    constexpr auto kTrackNumberAliases = std::array{std::string_view{"tn"}};
    constexpr auto kTrackTotalAliases = std::array{std::string_view{"tt"}};
    constexpr auto kDiscNumberAliases = std::array{std::string_view{"dn"}};
    constexpr auto kDiscTotalAliases = std::array{std::string_view{"td"}};
    constexpr auto kCoverArtAliases = std::array{std::string_view{"ca"}};

    constexpr auto kDurationAliases = std::array{std::string_view{"l"}};
    constexpr auto kBitrateAliases = std::array{std::string_view{"br"}};
    constexpr auto kSampleRateAliases = std::array{std::string_view{"sr"}};
    constexpr auto kBitDepthAliases = std::array{std::string_view{"bd"}};

    constexpr auto kMetadataSpecs = std::array{
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::Title,
                                  .canonicalName = "title",
                                  .aliases = std::span{kTitleAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::ArtistId,
                                  .canonicalName = "artist",
                                  .aliases = std::span{kArtistAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::AlbumId,
                                  .canonicalName = "album",
                                  .aliases = std::span{kAlbumAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::AlbumArtistId,
                                  .canonicalName = "albumArtist",
                                  .aliases = std::span{kAlbumArtistAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::ComposerId,
                                  .canonicalName = "composer",
                                  .aliases = std::span{kComposerAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::WorkId,
                                  .canonicalName = "work",
                                  .aliases = std::span{kWorkAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::GenreId,
                                  .canonicalName = "genre",
                                  .aliases = std::span{kGenreAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::Year,
                                  .canonicalName = "year",
                                  .aliases = std::span{kYearAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::TrackNumber,
                                  .canonicalName = "trackNumber",
                                  .aliases = std::span{kTrackNumberAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::TrackTotal,
                                  .canonicalName = "trackTotal",
                                  .aliases = std::span{kTrackTotalAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::DiscNumber,
                                  .canonicalName = "discNumber",
                                  .aliases = std::span{kDiscNumberAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::DiscTotal,
                                  .canonicalName = "discTotal",
                                  .aliases = std::span{kDiscTotalAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Metadata,
                                  .field = Field::CoverArtId,
                                  .canonicalName = "coverArt",
                                  .aliases = std::span{kCoverArtAliases}},
    };

    constexpr auto kPropertySpecs = std::array{
      QueryVariableCompletionSpec{.type = VariableType::Property,
                                  .field = Field::Duration,
                                  .canonicalName = "duration",
                                  .aliases = std::span{kDurationAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Property,
                                  .field = Field::Bitrate,
                                  .canonicalName = "bitrate",
                                  .aliases = std::span{kBitrateAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Property,
                                  .field = Field::SampleRate,
                                  .canonicalName = "sampleRate",
                                  .aliases = std::span{kSampleRateAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Property,
                                  .field = Field::Channels,
                                  .canonicalName = "channels",
                                  .aliases = std::span{kNoAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Property,
                                  .field = Field::BitDepth,
                                  .canonicalName = "bitDepth",
                                  .aliases = std::span{kBitDepthAliases}},
      QueryVariableCompletionSpec{.type = VariableType::Property,
                                  .field = Field::Codec,
                                  .canonicalName = "codec",
                                  .aliases = std::span{kNoAliases}},
    };

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
} // namespace ao::query
