// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/rt/TrackField.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt
{
  namespace
  {
    using F = TrackField;
    using Cat = TrackFieldCategory;
    using Q = query::Field;
    using Vk = TrackFieldValueKind;

    constexpr auto kDefinitions = std::to_array<TrackFieldDefinition>({
      // --- Metadata: text ---
      {
        .field = F::Title,
        .id = "title",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .optSortField = TrackSortField::Title,
        .optQueryField = Q::Title,
      },
      {
        .field = F::Artist,
        .id = "artist",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Artist,
        .optGroupKey = TrackGroupKey::Artist,
        .optQueryField = Q::ArtistId,
      },
      {
        .field = F::Album,
        .id = "album",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Album,
        .optGroupKey = TrackGroupKey::Album,
        .optQueryField = Q::AlbumId,
      },
      {
        .field = F::AlbumArtist,
        .id = "album-artist",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::AlbumArtist,
        .optGroupKey = TrackGroupKey::AlbumArtist,
        .optQueryField = Q::AlbumArtistId,
      },
      {
        .field = F::Genre,
        .id = "genre",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Genre,
        .optGroupKey = TrackGroupKey::Genre,
        .optQueryField = Q::GenreId,
      },
      {
        .field = F::Composer,
        .id = "composer",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Composer,
        .optGroupKey = TrackGroupKey::Composer,
        .optQueryField = Q::ComposerId,
      },
      {
        .field = F::Conductor,
        .id = "conductor",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Conductor,
        .optGroupKey = TrackGroupKey::Conductor,
        .optQueryField = Q::ConductorId,
      },
      {
        .field = F::Ensemble,
        .id = "ensemble",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Ensemble,
        .optGroupKey = TrackGroupKey::Ensemble,
        .optQueryField = Q::EnsembleId,
      },
      {
        .field = F::Work,
        .id = "work",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Work,
        .optGroupKey = TrackGroupKey::Work,
        .optQueryField = Q::WorkId,
      },
      {
        .field = F::Movement,
        .id = "movement",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = false,
        .valueCompletion = true,
        .optSortField = TrackSortField::Movement,
        .optQueryField = Q::MovementId,
      },
      {
        .field = F::Soloist,
        .id = "soloist",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = false,
        .valueCompletion = true,
        .optSortField = TrackSortField::Soloist,
        .optQueryField = Q::SoloistId,
      },
      // --- Metadata: number ---
      {
        .field = F::Year,
        .id = "year",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .optSortField = TrackSortField::Year,
        .optGroupKey = TrackGroupKey::Year,
        .optQueryField = Q::Year,
      },
      {
        .field = F::DiscNumber,
        .id = "disc-number",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .optSortField = TrackSortField::DiscNumber,
        .optQueryField = Q::DiscNumber,
      },
      {
        .field = F::DiscTotal,
        .id = "disc-total",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .optQueryField = Q::DiscTotal,
      },
      {
        .field = F::TrackNumber,
        .id = "track-number",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .optSortField = TrackSortField::TrackNumber,
        .optQueryField = Q::TrackNumber,
      },
      {
        .field = F::TrackTotal,
        .id = "track-total",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .optQueryField = Q::TrackTotal,
      },
      {
        .field = F::MovementNumber,
        .id = "movement-number",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .optSortField = TrackSortField::Movement,
        .optQueryField = Q::MovementNumber,
      },
      {
        .field = F::MovementTotal,
        .id = "movement-total",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .optQueryField = Q::MovementTotal,
      },
      // --- Duration ---
      {
        .field = F::Duration,
        .id = "duration",
        .category = Cat::Technical,
        .valueKind = Vk::Duration,
        .presentable = true,
        .sortable = true,
        .optSortField = TrackSortField::Duration,
        .optQueryField = Q::Duration,
      },
      // --- Tags ---
      {
        .field = F::Tags,
        .id = "tags",
        .category = Cat::Tag,
        .valueKind = Vk::TagList,
        .presentable = true,
      },
      // --- Technical ---
      {
        .field = F::FilePath,
        .id = "file-path",
        .category = Cat::Technical,
        .valueKind = Vk::FilePath,
        .presentable = true,
      },
      {
        .field = F::Codec,
        .id = "codec",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::Codec,
      },
      {
        .field = F::SampleRate,
        .id = "sample-rate",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::SampleRate,
      },
      {
        .field = F::Channels,
        .id = "channels",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::Channels,
      },
      {
        .field = F::BitDepth,
        .id = "bit-depth",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::BitDepth,
      },
      {
        .field = F::Bitrate,
        .id = "bitrate",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::Bitrate,
      },
      {
        .field = F::FileSize,
        .id = "file-size",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
      },
      {
        .field = F::ModifiedTime,
        .id = "modified-time",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
      },
      // --- Synthetic ---
      {
        .field = F::DisplayTrackNumber,
        .id = "display-track-number",
        .category = Cat::Synthetic,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .synthetic = true,
      },
      {
        .field = F::TechnicalSummary,
        .id = "technical-summary",
        .category = Cat::Synthetic,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .synthetic = true,
      },
      {
        .field = F::Quality,
        .id = "quality",
        .category = Cat::Synthetic,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .synthetic = true,
      },
    });

    static_assert(kDefinitions.size() == kTrackFieldCount, "Track Field registry must match kTrackFieldCount");

    constexpr auto kSortFieldIds = std::to_array<std::pair<TrackSortField, std::string_view>>({
      {TrackSortField::Artist, "artist"},
      {TrackSortField::Album, "album"},
      {TrackSortField::AlbumArtist, "album-artist"},
      {TrackSortField::Genre, "genre"},
      {TrackSortField::Composer, "composer"},
      {TrackSortField::Conductor, "conductor"},
      {TrackSortField::Ensemble, "ensemble"},
      {TrackSortField::Work, "work"},
      {TrackSortField::Movement, "movement"},
      {TrackSortField::Soloist, "soloist"},
      {TrackSortField::Year, "year"},
      {TrackSortField::DiscNumber, "disc-number"},
      {TrackSortField::TrackNumber, "track-number"},
      {TrackSortField::Title, "title"},
      {TrackSortField::Duration, "duration"},
    });

    constexpr auto kGroupKeyIds = std::to_array<std::pair<TrackGroupKey, std::string_view>>({
      {TrackGroupKey::None, "none"},
      {TrackGroupKey::Artist, "artist"},
      {TrackGroupKey::Album, "album"},
      {TrackGroupKey::AlbumArtist, "album-artist"},
      {TrackGroupKey::Genre, "genre"},
      {TrackGroupKey::Composer, "composer"},
      {TrackGroupKey::Conductor, "conductor"},
      {TrackGroupKey::Ensemble, "ensemble"},
      {TrackGroupKey::Work, "work"},
      {TrackGroupKey::Year, "year"},
    });

    static_assert(kSortFieldIds.size() == kTrackSortFieldCount);
    static_assert(kGroupKeyIds.size() == kTrackGroupKeyCount);

    template<typename Enum, std::size_t Size>
    std::optional<Enum> enumFromId(std::array<std::pair<Enum, std::string_view>, Size> const& definitions,
                                   std::string_view id)
    {
      for (auto const& [value, stableId] : definitions)
      {
        if (stableId == id)
        {
          return value;
        }
      }

      return std::nullopt;
    }

    template<typename Enum, std::size_t Size>
    std::string_view enumId(std::array<std::pair<Enum, std::string_view>, Size> const& definitions, Enum value)
    {
      for (auto const& [candidate, stableId] : definitions)
      {
        if (candidate == value)
        {
          return stableId;
        }
      }

      return {};
    }

    query::QueryVariableDescriptor const* queryVariableDescriptor(TrackFieldDefinition const& definition)
    {
      if (!definition.optQueryField)
      {
        return nullptr;
      }

      return query::findQueryVariableDescriptor(*definition.optQueryField);
    }
  } // namespace

  std::span<TrackFieldDefinition const> trackFieldDefinitions()
  {
    return kDefinitions;
  }

  TrackFieldDefinition const* trackFieldDefinition(TrackField field)
  {
    // NOLINTNEXTLINE(readability-qualified-auto) -- std::array iterator representations differ across libraries.
    auto const it = std::ranges::find(kDefinitions, field, &TrackFieldDefinition::field);
    return it != kDefinitions.end() ? &*it : nullptr;
  }

  std::optional<TrackField> trackFieldFromId(std::string_view id)
  {
    // NOLINTNEXTLINE(readability-qualified-auto) -- std::array iterator representations differ across libraries.
    auto const it = std::ranges::find(kDefinitions, id, &TrackFieldDefinition::id);
    return it != kDefinitions.end() ? std::optional{it->field} : std::nullopt;
  }

  std::optional<TrackSortField> trackSortFieldFromId(std::string_view id)
  {
    return enumFromId(kSortFieldIds, id);
  }

  std::optional<TrackGroupKey> trackGroupKeyFromId(std::string_view id)
  {
    return enumFromId(kGroupKeyIds, id);
  }

  std::optional<TrackField> trackFieldFromQueryField(query::Field field)
  {
    // NOLINTNEXTLINE(readability-qualified-auto) -- std::array iterator representations differ across libraries.
    auto const iter = std::ranges::find_if(kDefinitions,
                                           [field](TrackFieldDefinition const& definition)
                                           { return definition.optQueryField && *definition.optQueryField == field; });
    return iter != kDefinitions.end() ? std::optional{iter->field} : std::nullopt;
  }

  std::string_view trackFieldId(TrackField field)
  {
    if (auto const* def = trackFieldDefinition(field); def != nullptr)
    {
      return def->id;
    }

    return {};
  }

  std::string_view trackSortFieldId(TrackSortField field)
  {
    return enumId(kSortFieldIds, field);
  }

  std::string_view trackGroupKeyId(TrackGroupKey key)
  {
    return enumId(kGroupKeyIds, key);
  }

  std::optional<query::Field> trackFieldQueryField(TrackField const field)
  {
    if (auto const* const def = trackFieldDefinition(field); def != nullptr)
    {
      return def->optQueryField;
    }

    return std::nullopt;
  }

  std::string trackFieldFilterExpressionVariable(TrackField const field)
  {
    auto const* const definition = trackFieldDefinition(field);

    if (definition == nullptr)
    {
      return {};
    }

    auto const* const descriptor = queryVariableDescriptor(*definition);

    if (descriptor == nullptr)
    {
      return {};
    }

    return query::variableDisplayName(descriptor->type, descriptor->canonicalName);
  }

  bool supportsTrackFieldFilterExpression(TrackField const field)
  {
    auto const* const definition = trackFieldDefinition(field);
    return definition != nullptr && queryVariableDescriptor(*definition) != nullptr;
  }

  bool supportsTrackFieldValueCompletion(TrackField const field)
  {
    auto const* const definition = trackFieldDefinition(field);

    if (definition == nullptr || !definition->valueCompletion)
    {
      return false;
    }

    auto const* const descriptor = queryVariableDescriptor(*definition);
    return descriptor != nullptr && query::isDictionaryField(descriptor->field);
  }
} // namespace ao::rt
