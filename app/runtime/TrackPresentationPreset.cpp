// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackPresentationPreset.h"

#include <algorithm>
#include <array>
#include <ranges>

namespace ao::rt
{
  namespace
  {
    using Field = TrackPresentationField;
    using Sort = TrackSortField;

    std::vector<TrackPresentationPreset> const& getBuiltinPresets()
    {
      static auto const presets =
        std::vector<TrackPresentationPreset>{
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "songs",
                .groupBy = TrackGroupKey::None,
                .sortBy =
                  {
                    TrackSortTerm{Sort::Artist, true},
                    TrackSortTerm{Sort::Album, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields = {Field::Title, Field::Artist, Field::Album, Field::Duration, Field::Tags},
                .redundantFields = {},
              },
            .label = "Songs",
            .description = "General-purpose song list.",
          },
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "albums",
                .groupBy = TrackGroupKey::Album,
                .sortBy =
                  {
                    TrackSortTerm{Sort::AlbumArtist, true},
                    TrackSortTerm{Sort::Album, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields = {Field::TrackNumber, Field::Title, Field::Duration, Field::Year, Field::Tags},
                .redundantFields = {Field::Album, Field::AlbumArtist},
              },
            .label = "Albums",
            .description = "Grouped by album with track-oriented columns.",
          },
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "artists",
                .groupBy = TrackGroupKey::Artist,
                .sortBy =
                  {
                    TrackSortTerm{Sort::Artist, true},
                    TrackSortTerm{Sort::Album, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields = {Field::Album, Field::TrackNumber, Field::Title, Field::Duration, Field::Tags},
                .redundantFields = {Field::Artist},
              },
            .label = "Artists",
            .description = "Grouped by artist with album-oriented columns.",
          },
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "album-artists",
                .groupBy = TrackGroupKey::AlbumArtist,
                .sortBy =
                  {
                    TrackSortTerm{Sort::AlbumArtist, true},
                    TrackSortTerm{Sort::Album, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields =
                  {Field::Album, Field::TrackNumber, Field::Title, Field::Artist, Field::Duration, Field::Year},
                .redundantFields = {Field::AlbumArtist},
              },
            .label = "Album Artists",
            .description = "Grouped by album artist.",
          },
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "classical-composers",
                .groupBy = TrackGroupKey::Composer,
                .sortBy =
                  {
                    TrackSortTerm{Sort::Composer, true},
                    TrackSortTerm{Sort::Work, true},
                    TrackSortTerm{Sort::Album, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields = {Field::Work, Field::Title, Field::Artist, Field::Album, Field::Duration, Field::Year},
                .redundantFields = {Field::Composer},
              },
            .label = "Classical: Composers",
            .description = "Grouped by composer with work-oriented columns.",
          },
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "classical-works",
                .groupBy = TrackGroupKey::Work,
                .sortBy =
                  {
                    TrackSortTerm{Sort::Composer, true},
                    TrackSortTerm{Sort::Work, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields = {Field::Composer, Field::Title, Field::Artist, Field::Album, Field::Duration},
                .redundantFields = {Field::Work},
              },
            .label = "Classical: Works",
            .description = "Grouped by work with composer-oriented columns.",
          },
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "genres",
                .groupBy = TrackGroupKey::Genre,
                .sortBy =
                  {
                    TrackSortTerm{Sort::Genre, true},
                    TrackSortTerm{Sort::Artist, true},
                    TrackSortTerm{Sort::Album, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields = {Field::Artist, Field::Album, Field::Title, Field::Duration, Field::Tags},
                .redundantFields = {Field::Genre},
              },
            .label = "Genres",
            .description = "Grouped by genre.",
          },
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "years",
                .groupBy = TrackGroupKey::Year,
                .sortBy =
                  {
                    TrackSortTerm{Sort::Year, true},
                    TrackSortTerm{Sort::Artist, true},
                    TrackSortTerm{Sort::Album, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields = {Field::Artist, Field::Album, Field::Title, Field::Duration, Field::Tags},
                .redundantFields = {Field::Year},
              },
            .label = "Years",
            .description = "Grouped by year.",
          },
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "tagging",
                .groupBy = TrackGroupKey::None,
                .sortBy =
                  {
                    TrackSortTerm{Sort::Artist, true},
                    TrackSortTerm{Sort::Album, true},
                    TrackSortTerm{Sort::DiscNumber, true},
                    TrackSortTerm{Sort::TrackNumber, true},
                    TrackSortTerm{Sort::Title, true},
                  },
                .visibleFields = {Field::Title, Field::Artist, Field::Album, Field::Genre, Field::Year, Field::Tags},
                .redundantFields = {},
              },
            .label = "Tagging",
            .description = "Flat list with genre, year, and tags for curation.",
          },
        };

      return presets;
    }
  } // namespace

  std::string_view trackPresentationFieldId(TrackPresentationField field)
  {
    switch (field)
    {
      case TrackPresentationField::Title: return "title";
      case TrackPresentationField::Artist: return "artist";
      case TrackPresentationField::Album: return "album";
      case TrackPresentationField::AlbumArtist: return "album-artist";
      case TrackPresentationField::Genre: return "genre";
      case TrackPresentationField::Composer: return "composer";
      case TrackPresentationField::Work: return "work";
      case TrackPresentationField::Year: return "year";
      case TrackPresentationField::DiscNumber: return "disc-number";
      case TrackPresentationField::TrackNumber: return "track-number";
      case TrackPresentationField::Duration: return "duration";
      case TrackPresentationField::Tags: return "tags";
    }

    return "title";
  }

  std::optional<TrackPresentationField> trackPresentationFieldFromId(std::string_view id)
  {
    if (id == "title") return TrackPresentationField::Title;
    if (id == "artist") return TrackPresentationField::Artist;
    if (id == "album") return TrackPresentationField::Album;
    if (id == "album-artist") return TrackPresentationField::AlbumArtist;
    if (id == "genre") return TrackPresentationField::Genre;
    if (id == "composer") return TrackPresentationField::Composer;
    if (id == "work") return TrackPresentationField::Work;
    if (id == "year") return TrackPresentationField::Year;
    if (id == "disc-number") return TrackPresentationField::DiscNumber;
    if (id == "track-number") return TrackPresentationField::TrackNumber;
    if (id == "duration") return TrackPresentationField::Duration;
    if (id == "tags") return TrackPresentationField::Tags;

    return std::nullopt;
  }

  std::span<TrackPresentationPreset const> builtinTrackPresentationPresets()
  {
    return getBuiltinPresets();
  }

  TrackPresentationPreset const* builtinTrackPresentationPreset(std::string_view id)
  {
    auto const& presets = getBuiltinPresets();
    auto const iter = std::ranges::find(presets, id, [](TrackPresentationPreset const& p) { return p.spec.id; });

    if (iter == presets.end())
    {
      return nullptr;
    }

    return &*iter;
  }

  TrackPresentationSpec defaultTrackPresentationSpec()
  {
    return getBuiltinPresets().front().spec;
  }

  TrackPresentationSpec normalizeTrackPresentationSpec(TrackPresentationSpec const& spec)
  {
    auto result = spec;

    if (result.id.empty())
    {
      result.id = kDefaultTrackPresentationId;
    }

    auto deduplicate = [](std::vector<TrackPresentationField>& fields)
    {
      auto seen = std::vector<TrackPresentationField>{};
      seen.reserve(fields.size());

      for (auto const field : fields)
      {
        if (!std::ranges::contains(seen, field))
        {
          seen.push_back(field);
        }
      }

      fields = std::move(seen);
    };

    deduplicate(result.visibleFields);
    deduplicate(result.redundantFields);

    return result;
  }

  TrackListPresentationState presentationStateFromSpec(TrackPresentationSpec const& spec)
  {
    return TrackListPresentationState{
      .presentationId = spec.id,
      .groupBy = spec.groupBy,
      .sortBy = spec.sortBy,
      .visibleFields = spec.visibleFields,
      .redundantFields = spec.redundantFields,
    };
  }

  TrackPresentationSpec presentationSpecFromState(TrackListPresentationState const& state)
  {
    return TrackPresentationSpec{
      .id = state.presentationId,
      .groupBy = state.groupBy,
      .sortBy = state.sortBy,
      .visibleFields = state.visibleFields,
      .redundantFields = state.redundantFields,
    };
  }
} // namespace ao::rt
