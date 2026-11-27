// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
      MessageId::TrackFieldRecordingDate,
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

    struct BuiltinPresetMessageIds final
    {
      MessageId label;
      MessageId description;
    };

    constexpr auto kBuiltinPresetMessageIds = std::to_array<BuiltinPresetMessageIds>({
      {.label = MessageId::TrackPresentationLibrary, .description = MessageId::TrackPresentationLibraryDescription},
      {.label = MessageId::TrackPresentationListOrder, .description = MessageId::TrackPresentationListOrderDescription},
      {.label = MessageId::TrackPresentationSongs, .description = MessageId::TrackPresentationSongsDescription},
      {.label = MessageId::TrackPresentationAlbums, .description = MessageId::TrackPresentationAlbumsDescription},
      {.label = MessageId::TrackPresentationArtists, .description = MessageId::TrackPresentationArtistsDescription},
      {.label = MessageId::TrackPresentationPerformers,
       .description = MessageId::TrackPresentationPerformersDescription},
      {.label = MessageId::TrackPresentationGenres, .description = MessageId::TrackPresentationGenresDescription},
      {.label = MessageId::TrackPresentationYears, .description = MessageId::TrackPresentationYearsDescription},
      {.label = MessageId::TrackPresentationClassicalComposers,
       .description = MessageId::TrackPresentationClassicalComposersDescription},
      {.label = MessageId::TrackPresentationClassicalConductors,
       .description = MessageId::TrackPresentationClassicalConductorsDescription},
      {.label = MessageId::TrackPresentationClassicalWorks,
       .description = MessageId::TrackPresentationClassicalWorksDescription},
      {.label = MessageId::TrackPresentationTagging, .description = MessageId::TrackPresentationTaggingDescription},
      {.label = MessageId::TrackPresentationTechnical, .description = MessageId::TrackPresentationTechnicalDescription},
    });
    static_assert(kBuiltinPresetMessageIds.size() == std::to_underlying(rt::BuiltinPreset::Technical) + 1);
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

  std::string_view trackSortFieldLabel(MessageCatalog const& catalog, rt::TrackSortField const field) noexcept
  {
    if (field == rt::TrackSortField::RecordingYear)
    {
      return requiredText(catalog, MessageId::TrackSortRecordingYear);
    }

    // Use the first field's label for comparison keys shared by multiple columns.
    auto const definitions = rt::trackFieldDefinitions();
    auto const iter = std::ranges::find(definitions, std::optional{field}, &rt::TrackFieldDefinition::optSortField);
    return iter != definitions.end() ? trackFieldLabel(catalog, iter->field) : std::string_view{};
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

  TrackPresentationText builtinTrackPresentation(MessageCatalog const& catalog, rt::BuiltinPreset const preset)
  {
    auto const& ids = kBuiltinPresetMessageIds.at(std::to_underlying(preset));
    return {
      .label = requiredText(catalog, ids.label),
      .description = requiredText(catalog, ids.description),
    };
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
                                     std::size_t const matchCount,
                                     std::optional<std::size_t> const optSampleCount,
                                     bool const isAllTracks,
                                     bool const localEmpty)
  {
    if (!expressionValid)
    {
      return std::string{requiredText(catalog, MessageId::SmartListInvalidFilter)};
    }

    auto const source = isAllTracks ? std::string_view{"library"} : std::string_view{"source"};

    if (matchCount == 0)
    {
      return localEmpty ? requiredFormat(catalog, MessageId::SmartListNoTracks, {{"source", source}})
                        : std::string{requiredText(catalog, MessageId::SmartListNoMatches)};
    }

    auto const scope = localEmpty ? source : std::string_view{"filtered"};

    if (!optSampleCount)
    {
      return requiredFormat(catalog, MessageId::SmartListMatchCount, {{"scope", scope}, {"count", matchCount}});
    }

    if (*optSampleCount < matchCount)
    {
      return requiredFormat(catalog,
                            MessageId::SmartListShowingFirstMatches,
                            {{"scope", scope}, {"visible", *optSampleCount}, {"count", matchCount}});
    }

    return localEmpty
             ? requiredFormat(catalog, MessageId::SmartListShowingSource, {{"source", source}, {"count", matchCount}})
             : requiredFormat(catalog, MessageId::SmartListShowingAllMatches, {{"count", matchCount}});
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
