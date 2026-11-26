// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/TrackPresentation.h>

#include <ao/Contract.h>
#include <ao/rt/TrackField.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    using F = TrackField;

    constexpr auto kPresetKeys = std::to_array<std::string_view>({
      "library",
      "list-order",
      "songs",
      "albums",
      "artists",
      "performers",
      "genres",
      "years",
      "classical-composers",
      "classical-conductors",
      "classical-works",
      "tagging",
      "technical",
    });
    static_assert(kPresetKeys.size() == std::to_underlying(BuiltinPreset::Technical) + 1);

    std::vector<TrackPresentationPreset> const& builtInPresets()
    {
      static auto const
        kPresets =
          std::vector{
            // --- Daily listening ---
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Library,
                  .groupBy = TrackGroupKey::None,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields = {F::DisplayTrackNumber, F::Title, F::Artist, F::Album, F::Year, F::Duration},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::ListOrder,
                  .groupBy = TrackGroupKey::None,
                  .visibleFields = {F::DisplayTrackNumber, F::Title, F::Artist, F::Album, F::Year, F::Duration},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Songs,
                  .groupBy = TrackGroupKey::None,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                    },
                  .visibleFields = {F::Title, F::Artist, F::Album, F::Duration, F::Year},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Albums,
                  .groupBy = TrackGroupKey::Album,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields = {F::DisplayTrackNumber, F::Title, F::Artist, F::Duration},
                  .redundantFields = {F::Album, F::AlbumArtist},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Artists,
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
                  .visibleFields = {F::Year, F::Album, F::DisplayTrackNumber, F::Title, F::Artist, F::Duration},
                  .redundantFields = {F::AlbumArtist},
                },
            },
            // --- Browsing and discovery ---
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Performers,
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
                  .visibleFields = {F::Year, F::Album, F::DisplayTrackNumber, F::Title, F::Duration},
                  .redundantFields = {F::Artist},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Genres,
                  .groupBy = TrackGroupKey::Genre,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::Genre, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields = {F::Artist, F::Album, F::DisplayTrackNumber, F::Title, F::Year, F::Duration},
                  .redundantFields = {F::Genre},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Years,
                  .groupBy = TrackGroupKey::Year,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields = {F::Artist, F::Album, F::DisplayTrackNumber, F::Title, F::Genre, F::Duration},
                  .redundantFields = {F::Year},
                },
            },
            // --- Classical ---
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::ClassicalComposers,
                  .groupBy = TrackGroupKey::Composer,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::Composer, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Work, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::RecordingYear, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Movement, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields = {F::Work,
                                    F::Movement,
                                    F::Artist,
                                    F::Album,
                                    F::Year,
                                    F::Duration,
                                    F::DisplayTrackNumber,
                                    F::RecordingDate},
                  .redundantFields = {F::Composer},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::ClassicalConductors,
                  .groupBy = TrackGroupKey::Conductor,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::Conductor, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Composer, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Work, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::RecordingYear, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Movement, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields = {F::Work,
                                    F::Movement,
                                    F::Composer,
                                    F::Conductor,
                                    F::Ensemble,
                                    F::Album,
                                    F::Year,
                                    F::Duration,
                                    F::RecordingDate},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::ClassicalWorks,
                  .groupBy = TrackGroupKey::Work,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::Composer, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Work, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::RecordingYear, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Year, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Movement, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields =
                    {F::DisplayTrackNumber, F::Movement, F::Artist, F::Album, F::Year, F::Duration, F::RecordingDate},
                  .redundantFields = {F::Composer, F::Work},
                },
            },
            // --- Maintenance ---
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Tagging,
                  .groupBy = TrackGroupKey::None,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::Artist, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields = {F::DiscNumber,
                                    F::TrackNumber,
                                    F::Title,
                                    F::Artist,
                                    F::Album,
                                    F::Genre,
                                    F::Year,
                                    F::Duration,
                                    F::Tags},
                },
            },
            TrackPresentationPreset{
              .spec =
                TrackPresentationSpec{
                  .presetId = BuiltinPreset::Technical,
                  .groupBy = TrackGroupKey::None,
                  .sortBy =
                    {
                      TrackSortTerm{.field = TrackSortField::AlbumArtist, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Album, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::TrackNumber, .ascending = true},
                      TrackSortTerm{.field = TrackSortField::Title, .ascending = true},
                    },
                  .visibleFields = {F::Title, F::Artist, F::Album, F::TechnicalSummary, F::FileSize, F::FilePath},
                },
            },
          };

      return kPresets;
    }
  } // namespace

  std::span<TrackPresentationPreset const> builtinTrackPresentationPresets()
  {
    return builtInPresets();
  }

  TrackPresentationPreset const& builtinTrackPresentationPreset(BuiltinPreset const id)
  {
    return builtInPresets().at(std::to_underlying(id));
  }

  TrackColumnSort TrackPresentationSpec::nextColumnSort(TrackField const column) const noexcept
  {
    return {
      .column = column, .ascending = !optColumnSort || optColumnSort->column != column || !optColumnSort->ascending};
  }

  std::vector<TrackSortTerm> TrackPresentationSpec::effectiveSortBy() const
  {
    if (!optColumnSort)
    {
      return sortBy;
    }

    auto const optSortField = trackSortFieldForColumn(optColumnSort->column);
    AO_EXPECTS(optSortField, "A column sort requires a sortable track column");
    auto terms = std::vector<TrackSortTerm>{};
    terms.reserve(sortBy.size() + 2);

    if (*optSortField == TrackSortField::TrackNumber)
    {
      terms.push_back({.field = TrackSortField::DiscNumber, .ascending = optColumnSort->ascending});
    }

    terms.push_back({.field = *optSortField, .ascending = optColumnSort->ascending});

    for (auto const& term : sortBy)
    {
      if (!std::ranges::contains(terms, term.field, &TrackSortTerm::field))
      {
        terms.push_back(term);
      }
    }

    return terms;
  }

  bool TrackPresentationSpec::hasOnlyColumnSortChange(TrackPresentationSpec const& other) const
  {
    if (optColumnSort == other.optColumnSort)
    {
      return false;
    }

    auto spec = *this;
    spec.optColumnSort = other.optColumnSort;
    return spec == other;
  }

  std::span<TrackSortField const> trackGroupSortFields(TrackGroupKey const groupBy) noexcept
  {
    static constexpr auto kArtist = std::to_array({TrackSortField::Artist});
    static constexpr auto kAlbum = std::to_array({TrackSortField::AlbumArtist, TrackSortField::Album});
    static constexpr auto kAlbumArtist = std::to_array({TrackSortField::AlbumArtist});
    static constexpr auto kGenre = std::to_array({TrackSortField::Genre});
    static constexpr auto kComposer = std::to_array({TrackSortField::Composer});
    static constexpr auto kConductor = std::to_array({TrackSortField::Conductor});
    static constexpr auto kEnsemble = std::to_array({TrackSortField::Ensemble});
    static constexpr auto kWork = std::to_array({TrackSortField::Composer, TrackSortField::Work});
    static constexpr auto kYear = std::to_array({TrackSortField::Year});

    switch (groupBy)
    {
      case TrackGroupKey::Artist: return kArtist;
      case TrackGroupKey::Album: return kAlbum;
      case TrackGroupKey::AlbumArtist: return kAlbumArtist;
      case TrackGroupKey::Genre: return kGenre;
      case TrackGroupKey::Composer: return kComposer;
      case TrackGroupKey::Conductor: return kConductor;
      case TrackGroupKey::Ensemble: return kEnsemble;
      case TrackGroupKey::Work: return kWork;
      case TrackGroupKey::Year: return kYear;
      case TrackGroupKey::None: return {};
    }

    return {};
  }

  std::vector<TrackSortTerm> trackGroupSortBy(TrackGroupKey const groupBy,
                                              std::span<TrackSortTerm const> const baseSortBy)
  {
    auto const groupFields = trackGroupSortFields(groupBy);

    if (groupFields.empty())
    {
      return {};
    }

    auto groupOrder = std::vector<TrackSortTerm>{};
    groupOrder.reserve(groupFields.size());

    for (auto const& term : baseSortBy)
    {
      if (std::ranges::contains(groupFields, term.field) &&
          !std::ranges::contains(groupOrder, term.field, &TrackSortTerm::field))
      {
        groupOrder.push_back(term);
      }
    }

    auto const fallbackAscending = groupOrder.empty() || groupOrder.front().ascending;

    for (auto const field : groupFields)
    {
      if (!std::ranges::contains(groupOrder, field, &TrackSortTerm::field))
      {
        groupOrder.push_back({.field = field, .ascending = fallbackAscending});
      }
    }

    return groupOrder;
  }

  std::string_view builtinPresetKey(BuiltinPreset const id)
  {
    return kPresetKeys.at(std::to_underlying(id));
  }

  std::string trackPresetKey(TrackPresetId const& id)
  {
    if (auto const* builtin = std::get_if<BuiltinPreset>(&id); builtin != nullptr)
    {
      return std::string{builtinPresetKey(*builtin)};
    }

    return std::format("{:016x}", std::get<CustomPresetId>(id).raw());
  }

  std::optional<TrackPresetId> parseTrackPresetId(std::string_view const key) noexcept
  {
    for (std::size_t index = 0; index < kPresetKeys.size(); ++index)
    {
      if (kPresetKeys[index] == key)
      {
        return static_cast<BuiltinPreset>(index);
      }
    }

    if (key.size() != 16 || key.find_first_not_of("0123456789abcdef") != std::string_view::npos)
    {
      return std::nullopt;
    }

    std::uint64_t number = 0;

    if (auto const [end, error] = std::from_chars(key.data(), key.data() + key.size(), number, 16);
        error != std::errc{} || end != key.data() + key.size())
    {
      return std::nullopt;
    }

    return CustomPresetId{number};
  }

  TrackPresentationSpec defaultTrackPresentationSpec()
  {
    return builtinTrackPresentationPreset(BuiltinPreset::Library).spec;
  }

  TrackPresentationSpec normalizeTrackPresentationSpec(TrackPresentationSpec const& spec)
  {
    auto result = TrackPresentationSpec{spec};

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

    if (result.visibleFields.empty())
    {
      result.visibleFields = {TrackField::Title};
    }

    deduplicate(result.redundantFields);
    // Primary-name headings cannot replace the row's additional-credit count.
    std::erase_if(result.redundantFields,
                  [](TrackField field)
                  { return field == F::Conductor || field == F::Ensemble || field == F::Soloist; });

    return result;
  }
} // namespace ao::rt
