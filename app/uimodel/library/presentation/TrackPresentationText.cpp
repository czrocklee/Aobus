// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  namespace
  {
    using i18n::MessageCatalog;
    using i18n::MessageId;

    constexpr auto kTrackFieldMessageIds = std::to_array<MessageId>({
      MessageId::TrackFieldTitle,
      MessageId::TrackFieldArtist,
      MessageId::TrackFieldAlbum,
      MessageId::TrackFieldAlbumArtist,
      MessageId::TrackFieldGenre,
      MessageId::TrackFieldComposer,
      MessageId::TrackFieldConductor,
      MessageId::TrackFieldEnsemble,
      MessageId::TrackFieldWork,
      MessageId::TrackFieldMovement,
      MessageId::TrackFieldSoloist,
      MessageId::TrackFieldYear,
      MessageId::TrackFieldDiscNumber,
      MessageId::TrackFieldDiscTotal,
      MessageId::TrackFieldTrackNumber,
      MessageId::TrackFieldTrackTotal,
      MessageId::TrackFieldMovementNumber,
      MessageId::TrackFieldMovementTotal,
      MessageId::TrackFieldDuration,
      MessageId::TrackFieldTags,
      MessageId::TrackFieldFilePath,
      MessageId::TrackFieldCodec,
      MessageId::TrackFieldSampleRate,
      MessageId::TrackFieldChannels,
      MessageId::TrackFieldBitDepth,
      MessageId::TrackFieldBitrate,
      MessageId::TrackFieldFileSize,
      MessageId::TrackFieldModifiedTime,
      MessageId::TrackFieldDisplayTrackNumber,
      MessageId::TrackFieldTechnicalSummary,
      MessageId::TrackFieldQuality,
    });

    static_assert(kTrackFieldMessageIds.size() == rt::kTrackFieldCount);

    constexpr auto kMissingTrackValueMessageIds = std::to_array<MessageId>({
      MessageId::MissingTrackArtist,
      MessageId::MissingTrackAlbum,
      MessageId::MissingTrackYear,
      MessageId::MissingTrackGenre,
      MessageId::MissingTrackComposer,
      MessageId::MissingTrackConductor,
      MessageId::MissingTrackEnsemble,
      MessageId::MissingTrackWork,
    });

    static_assert(kMissingTrackValueMessageIds.size() == rt::kMissingTrackValueKindCount);

    struct BuiltinTrackPresentationDefinition final
    {
      std::string_view id;
      MessageId labelId;
      MessageId descriptionId;
    };

    constexpr auto kBuiltinTrackPresentationDefinitions = std::to_array<BuiltinTrackPresentationDefinition>({
      {.id = "library",
       .labelId = MessageId::TrackPresentationLibrary,
       .descriptionId = MessageId::TrackPresentationLibraryDescription},
      {.id = "list-order",
       .labelId = MessageId::TrackPresentationListOrder,
       .descriptionId = MessageId::TrackPresentationListOrderDescription},
      {.id = "songs",
       .labelId = MessageId::TrackPresentationSongs,
       .descriptionId = MessageId::TrackPresentationSongsDescription},
      {.id = "albums",
       .labelId = MessageId::TrackPresentationAlbums,
       .descriptionId = MessageId::TrackPresentationAlbumsDescription},
      {.id = "artists",
       .labelId = MessageId::TrackPresentationArtists,
       .descriptionId = MessageId::TrackPresentationArtistsDescription},
      {.id = "performers",
       .labelId = MessageId::TrackPresentationPerformers,
       .descriptionId = MessageId::TrackPresentationPerformersDescription},
      {.id = "genres",
       .labelId = MessageId::TrackPresentationGenres,
       .descriptionId = MessageId::TrackPresentationGenresDescription},
      {.id = "years",
       .labelId = MessageId::TrackPresentationYears,
       .descriptionId = MessageId::TrackPresentationYearsDescription},
      {.id = "classical-composers",
       .labelId = MessageId::TrackPresentationClassicalComposers,
       .descriptionId = MessageId::TrackPresentationClassicalComposersDescription},
      {.id = "classical-conductors",
       .labelId = MessageId::TrackPresentationClassicalConductors,
       .descriptionId = MessageId::TrackPresentationClassicalConductorsDescription},
      {.id = "classical-works",
       .labelId = MessageId::TrackPresentationClassicalWorks,
       .descriptionId = MessageId::TrackPresentationClassicalWorksDescription},
      {.id = "tagging",
       .labelId = MessageId::TrackPresentationTagging,
       .descriptionId = MessageId::TrackPresentationTaggingDescription},
      {.id = "technical",
       .labelId = MessageId::TrackPresentationTechnical,
       .descriptionId = MessageId::TrackPresentationTechnicalDescription},
    });
  } // namespace
  using i18n::MessageCatalog;
  using i18n::MessageId;
  using i18n::requiredFormat;
  using i18n::requiredText;

  std::string_view trackFieldLabel(MessageCatalog const& catalog, rt::TrackField const field) noexcept
  {
    auto const index = static_cast<std::size_t>(field);
    return index < kTrackFieldMessageIds.size() ? requiredText(catalog, kTrackFieldMessageIds[index])
                                                : std::string_view{};
  }

  std::string_view trackGroupKeyLabel(MessageCatalog const& catalog, rt::TrackGroupKey const key) noexcept
  {
    switch (key)
    {
      case rt::TrackGroupKey::None: return requiredText(catalog, MessageId::TrackGroupNone);
      case rt::TrackGroupKey::Artist: return trackFieldLabel(catalog, rt::TrackField::Artist);
      case rt::TrackGroupKey::Album: return trackFieldLabel(catalog, rt::TrackField::Album);
      case rt::TrackGroupKey::AlbumArtist: return trackFieldLabel(catalog, rt::TrackField::AlbumArtist);
      case rt::TrackGroupKey::Genre: return trackFieldLabel(catalog, rt::TrackField::Genre);
      case rt::TrackGroupKey::Composer: return trackFieldLabel(catalog, rt::TrackField::Composer);
      case rt::TrackGroupKey::Conductor: return trackFieldLabel(catalog, rt::TrackField::Conductor);
      case rt::TrackGroupKey::Ensemble: return trackFieldLabel(catalog, rt::TrackField::Ensemble);
      case rt::TrackGroupKey::Work: return trackFieldLabel(catalog, rt::TrackField::Work);
      case rt::TrackGroupKey::Year: return trackFieldLabel(catalog, rt::TrackField::Year);
    }

    return {};
  }

  std::string_view missingTrackValueLabel(MessageCatalog const& catalog, rt::MissingTrackValueKind const kind) noexcept
  {
    auto const index = static_cast<std::size_t>(kind);
    return index < kMissingTrackValueMessageIds.size() ? requiredText(catalog, kMissingTrackValueMessageIds[index])
                                                       : std::string_view{};
  }

  std::optional<TrackPresentationText> builtinTrackPresentation(MessageCatalog const& catalog,
                                                                std::string_view const id) noexcept
  {
    for (auto const& definition : kBuiltinTrackPresentationDefinitions)
    {
      if (definition.id == id)
      {
        return TrackPresentationText{
          .label = requiredText(catalog, definition.labelId),
          .description = requiredText(catalog, definition.descriptionId),
        };
      }
    }

    return std::nullopt;
  }

  std::string completionDetail(MessageCatalog const& catalog, rt::CompletionDetail const& detail)
  {
    switch (detail.kind)
    {
      case rt::CompletionDetailKind::None: return {};
      case rt::CompletionDetailKind::ResolvedText: return detail.resolvedText;
      case rt::CompletionDetailKind::Field: return std::string{requiredText(catalog, MessageId::CompletionField)};
      case rt::CompletionDetailKind::Alias: return std::string{requiredText(catalog, MessageId::CompletionAlias)};
      case rt::CompletionDetailKind::Operator: return std::string{requiredText(catalog, MessageId::CompletionOperator)};
      case rt::CompletionDetailKind::LogicalOperator:
        return std::string{requiredText(catalog, MessageId::CompletionLogicalOperator)};
      case rt::CompletionDetailKind::Frequency: return std::to_string(detail.frequency);
    }

    return {};
  }

  std::string trackSelectionSummary(MessageCatalog const& catalog,
                                    std::size_t const count,
                                    std::string_view const duration)
  {
    if (count == 0)
    {
      return {};
    }

    return requiredFormat(catalog,
                          MessageId::TrackSelectionSummary,
                          {{"count", count}, {"hasDuration", duration.empty() ? "no" : "yes"}, {"duration", duration}});
  }

  std::string smartListMembershipEditingText(MessageCatalog const& catalog,
                                             bool const direct,
                                             std::string_view const expression)
  {
    if (!direct)
    {
      return std::string{requiredText(catalog, MessageId::SmartListMembershipComputed)};
    }

    return requiredFormat(catalog, MessageId::SmartListMembershipDirect, {{"expression", expression}});
  }

  std::string smartListPreviewStatus(MessageCatalog const& catalog,
                                     bool const expressionValid,
                                     std::size_t const count,
                                     bool const isAllTracks,
                                     bool const localEmpty)
  {
    if (localEmpty)
    {
      auto const source = isAllTracks ? std::string_view{"library"} : std::string_view{"source"};

      if (count == 0)
      {
        return requiredFormat(catalog, MessageId::SmartListNoTracks, {{"source", source}});
      }

      return requiredFormat(catalog, MessageId::SmartListShowingSource, {{"source", source}, {"count", count}});
    }

    if (!expressionValid)
    {
      return std::string{requiredText(catalog, MessageId::SmartListInvalidFilter)};
    }

    if (count == 0)
    {
      return std::string{requiredText(catalog, MessageId::SmartListNoMatches)};
    }

    constexpr std::size_t kMaxPreview = 10;

    if (count <= kMaxPreview)
    {
      return requiredFormat(catalog, MessageId::SmartListShowingAllMatches, {{"count", count}});
    }

    return requiredFormat(
      catalog, MessageId::SmartListShowingFirstMatches, {{"visible", kMaxPreview}, {"count", count}});
  }

  std::string trackChannelText(MessageCatalog const& catalog, std::uint8_t const channels)
  {
    if (channels == 1)
    {
      return std::string{requiredText(catalog, MessageId::TrackChannelMono)};
    }

    if (channels == 2)
    {
      return std::string{requiredText(catalog, MessageId::TrackChannelStereo)};
    }

    return requiredFormat(catalog, MessageId::TrackChannelCount, {{"count", channels}});
  }
} // namespace ao::uimodel
