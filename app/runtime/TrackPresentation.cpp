// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    using F = TrackField;

    std::vector<TrackPresentationPreset> const& getBuiltinPresets()
    {
      static auto const presets =
        std::vector{
          TrackPresentationPreset{
            .spec =
              TrackPresentationSpec{
                .id = "songs",
                .groupBy = TrackGroupKey::None,
                .sortBy =
                  {
                    TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields = {F::DisplayTrackNumber, F::Title, F::Artist, F::Album, F::Year, F::Duration, F::Tags},
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
                    TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields = {F::DisplayTrackNumber, F::Title, F::Artist, F::Duration, F::Tags},
                .redundantFields = {F::Album, F::AlbumArtist},
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
                    TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields = {F::Year, F::Album, F::DisplayTrackNumber, F::Title, F::Duration, F::Tags},
                .redundantFields = {F::Artist},
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
                    TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields = {F::Album, F::DisplayTrackNumber, F::Title, F::Artist, F::Duration, F::Year},
                .redundantFields = {F::AlbumArtist},
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
                    TrackSortTerm{.field = TrackSortField::Composer, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Work, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Movement, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields =
                  {F::DisplayTrackNumber, F::Work, F::Movement, F::Artist, F::Album, F::Year, F::Duration},
                .redundantFields = {F::Composer},
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
                    TrackSortTerm{.field = TrackSortField::Composer, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Work, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Movement, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields = {F::DisplayTrackNumber, F::Movement, F::Artist, F::Album, F::Year, F::Duration},
                .redundantFields = {F::Composer, F::Work},
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
                    TrackSortTerm{.field = TrackSortField::Genre, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields = {F::Artist, F::Album, F::DisplayTrackNumber, F::Title, F::Year, F::Duration, F::Tags},
                .redundantFields = {F::Genre},
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
                    TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Genre, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields = {F::Artist, F::Album, F::DisplayTrackNumber, F::Title, F::Genre, F::Duration, F::Tags},
                .redundantFields = {F::Year},
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
                    TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                    TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                  },
                .visibleFields = {F::Title, F::Artist, F::Album, F::Genre, F::Year, F::Tags},
                .redundantFields = {},
              },
            .label = "Tagging",
            .description = "Flat list with genre, year, and tags for curation.",
          },
        };

      return presets;
    }
  } // namespace

  std::span<TrackPresentationPreset const> builtinTrackPresentationPresets()
  {
    return getBuiltinPresets();
  }

  TrackPresentationPreset const* builtinTrackPresentationPreset(std::string_view id)
  {
    auto const& presets = getBuiltinPresets();
    auto const iter =
      std::ranges::find(presets, id, [](TrackPresentationPreset const& preset) { return preset.spec.id; });

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
    auto result = TrackPresentationSpec{spec};

    if (result.id.empty())
    {
      result.id = kDefaultTrackPresentationId;
    }

    auto deduplicate = [](std::vector<TrackField>& fields)
    {
      auto seen = std::vector<TrackField>{};
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
} // namespace ao::rt
