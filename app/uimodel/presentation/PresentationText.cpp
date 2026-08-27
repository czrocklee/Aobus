// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/presentation/PresentationText.h>

#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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

  AudioBackendPresentation audioBackendPresentation(MessageCatalog const& catalog, audio::BackendId const& id)
  {
    if (id == audio::kBackendPipeWire)
    {
      return AudioBackendPresentation{
        .label = "PipeWire",
        .description = std::string{requiredText(catalog, MessageId::AudioBackendPipeWireDescription)},
        .shortLabel = "PW",
        .outputDeviceDescriptionFallback = "PipeWire",
        .iconKind = AudioIconKind::AudioServer,
      };
    }

    if (id == audio::kBackendAlsa)
    {
      return AudioBackendPresentation{
        .label = "ALSA",
        .description = std::string{requiredText(catalog, MessageId::AudioBackendAlsaDescription)},
        .shortLabel = "ALSA",
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    if (id == audio::kBackendWasapi)
    {
      return AudioBackendPresentation{
        .label = "WASAPI",
        .description = std::string{requiredText(catalog, MessageId::AudioBackendWasapiDescription)},
        .shortLabel = "WASAPI",
        .outputDeviceDescriptionFallback =
          std::string{requiredText(catalog, MessageId::AudioBackendWasapiOutputFallback)},
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    if (id == audio::kBackendCoreAudio)
    {
      return AudioBackendPresentation{
        .label = "Core Audio",
        .description = std::string{requiredText(catalog, MessageId::AudioBackendCoreAudioDescription)},
        .shortLabel = "Core Audio",
        .outputDeviceDescriptionFallback =
          std::string{requiredText(catalog, MessageId::AudioBackendCoreAudioOutputFallback)},
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    auto const& fallback = id.raw();
    return AudioBackendPresentation{.label = std::string{fallback}, .shortLabel = std::string{fallback}};
  }

  AudioProfilePresentation audioProfilePresentation(MessageCatalog const& catalog, audio::ProfileId const& id)
  {
    if (id == audio::kProfileShared)
    {
      return AudioProfilePresentation{
        .label = std::string{requiredText(catalog, MessageId::AudioProfileShared)},
        .description = std::string{requiredText(catalog, MessageId::AudioProfileSharedDescription)},
      };
    }

    if (id == audio::kProfileExclusive)
    {
      return AudioProfilePresentation{
        .label = std::string{requiredText(catalog, MessageId::AudioProfileExclusive)},
        .description = std::string{requiredText(catalog, MessageId::AudioProfileExclusiveDescription)},
      };
    }

    return AudioProfilePresentation{.label = std::string{id.raw()}};
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

  std::string notificationMessage(MessageCatalog const& catalog, rt::NotificationMessage const& message)
  {
    auto const* report = std::get_if<rt::NotificationReport>(&message);

    if (report == nullptr)
    {
      return std::get<std::string>(message);
    }

    auto const failureReason =
      report->detail.empty() ? std::string{requiredText(catalog, MessageId::NotificationUnknownError)} : report->detail;
    auto const trackLabel = [&]
    {
      if (!report->subject.empty())
      {
        return report->subject;
      }

      if (report->trackId != kInvalidTrackId)
      {
        return requiredFormat(catalog, MessageId::NotificationTrackLabel, {{"id", report->trackId.raw()}});
      }

      return std::string{requiredText(catalog, MessageId::NotificationPlaybackSubject)};
    };

    switch (report->templateId)
    {
      case rt::NotificationReportTemplate::PlaybackTrackOpenFailed:
      {
        auto const track = trackLabel();
        return requiredFormat(
          catalog, MessageId::NotificationTrackOpenFailed, {{"track", track}, {"reason", failureReason}});
      }
      case rt::NotificationReportTemplate::PlaybackDecodeFailed:
      {
        auto const track = trackLabel();
        return requiredFormat(
          catalog, MessageId::NotificationDecodeFailed, {{"track", track}, {"reason", failureReason}});
      }
      case rt::NotificationReportTemplate::PlaybackRouteActivationFailed:
        return requiredFormat(catalog, MessageId::NotificationRouteActivationFailed, {{"reason", failureReason}});
      case rt::NotificationReportTemplate::PlaybackDeviceLost:
        return requiredFormat(catalog, MessageId::NotificationDeviceLost, {{"reason", failureReason}});
      case rt::NotificationReportTemplate::PlaybackSequenceFinished:
        return std::string{requiredText(catalog, MessageId::NotificationSequenceFinished)};
      case rt::NotificationReportTemplate::PlaybackTracksSkipped:
        return requiredFormat(catalog, MessageId::NotificationTracksSkipped, {{"count", report->count}});
      case rt::NotificationReportTemplate::PlaybackStoppedAfterFailures:
        return requiredFormat(catalog, MessageId::NotificationStoppedAfterFailures, {{"count", report->count}});
      case rt::NotificationReportTemplate::PlaybackStoppedForTrack:
        return requiredFormat(catalog,
                              MessageId::NotificationStoppedForTrack,
                              {{"hasSubject", report->subject.empty() ? "no" : "yes"},
                               {"hasDetail", report->detail.empty() ? "no" : "yes"},
                               {"subject", report->subject},
                               {"detail", report->detail}});
    }

    return std::string{requiredText(catalog, MessageId::NotificationFallback)};
  }

  std::string notificationGroupMessage(MessageCatalog const& catalog,
                                       rt::NotificationSeverity const severity,
                                       std::size_t const count)
  {
    auto messageId = MessageId::NotificationGroupedInfo;

    switch (severity)
    {
      case rt::NotificationSeverity::Info: messageId = MessageId::NotificationGroupedInfo; break;
      case rt::NotificationSeverity::Warning: messageId = MessageId::NotificationGroupedWarning; break;
      case rt::NotificationSeverity::Error: messageId = MessageId::NotificationGroupedError; break;
    }

    return requiredFormat(catalog, messageId, {{"count", count}});
  }

  std::string libraryTaskProgressDetail(MessageCatalog const& catalog,
                                        rt::LibraryTaskProgressKind const kind,
                                        std::string_view const subject)
  {
    auto activityId = MessageId::LibraryTaskScanning;

    switch (kind)
    {
      case rt::LibraryTaskProgressKind::Scanning: activityId = MessageId::LibraryTaskScanning; break;
      case rt::LibraryTaskProgressKind::Updating: activityId = MessageId::LibraryTaskUpdating; break;
      case rt::LibraryTaskProgressKind::Fingerprinting: activityId = MessageId::LibraryTaskFingerprinting; break;
      case rt::LibraryTaskProgressKind::IndexingAudioIdentity:
        activityId = MessageId::LibraryTaskIndexingAudioIdentity;
        break;
      case rt::LibraryTaskProgressKind::PreparingImport: activityId = MessageId::LibraryTaskPreparingImport; break;
      case rt::LibraryTaskProgressKind::Importing: activityId = MessageId::LibraryTaskImporting; break;
      case rt::LibraryTaskProgressKind::Exporting: activityId = MessageId::LibraryTaskExporting; break;
    }

    auto const activity = requiredText(catalog, activityId);
    return requiredFormat(
      catalog,
      MessageId::LibraryTaskProgressDetail,
      {{"hasSubject", subject.empty() ? "no" : "yes"}, {"activity", activity}, {"subject", subject}});
  }

  std::string libraryTaskProgressCompact(MessageCatalog const& catalog,
                                         rt::LibraryTaskProgressKind const kind,
                                         std::string_view const subject)
  {
    switch (kind)
    {
      case rt::LibraryTaskProgressKind::Scanning:
        return std::string{requiredText(catalog, MessageId::LibraryTaskScanningCompact)};
      case rt::LibraryTaskProgressKind::Updating:
        return std::string{requiredText(catalog, MessageId::LibraryTaskUpdatingCompact)};
      case rt::LibraryTaskProgressKind::Fingerprinting:
      case rt::LibraryTaskProgressKind::IndexingAudioIdentity:
      case rt::LibraryTaskProgressKind::PreparingImport:
      case rt::LibraryTaskProgressKind::Importing:
      case rt::LibraryTaskProgressKind::Exporting: return libraryTaskProgressDetail(catalog, kind, subject);
    }

    return {};
  }

  std::string formatLibraryScanMessage(MessageCatalog const& catalog, LibraryScanOutcome const& outcome)
  {
    auto const changeSummary = [&]
    {
      auto relinked = std::string{};

      if (outcome.relinkedCount > 0)
      {
        relinked = requiredFormat(catalog, MessageId::LibraryScanRelinked, {{"count", outcome.relinkedCount}});
      }

      auto missing = std::string{};

      if (outcome.missingCount > 0)
      {
        missing = requiredFormat(catalog, MessageId::LibraryScanMissing, {{"count", outcome.missingCount}});
      }

      if (!relinked.empty() && !missing.empty())
      {
        return requiredFormat(
          catalog, MessageId::LibraryScanChangesCombined, {{"relinked", relinked}, {"missing", missing}});
      }

      if (!relinked.empty())
      {
        return relinked;
      }

      return missing;
    };

    switch (outcome.verdict)
    {
      case LibraryScanVerdict::UpToDate: return std::string{requiredText(catalog, MessageId::LibraryScanUpToDate)};
      case LibraryScanVerdict::Complete:
      case LibraryScanVerdict::NeedsReview:
      {
        auto changes = changeSummary();
        return changes.empty() ? std::string{requiredText(catalog, MessageId::LibraryScanComplete)}
                               : std::move(changes);
      }
      case LibraryScanVerdict::CompletedWithErrors:
      {
        auto const changes = changeSummary();
        return requiredFormat(catalog,
                              MessageId::LibraryScanCompletedWithErrors,
                              {{"hasChanges", changes.empty() ? "no" : "yes"}, {"changes", changes}});
      }
      case LibraryScanVerdict::Unreadable:
      {
        return requiredFormat(catalog, MessageId::LibraryScanUnreadable, {{"count", outcome.failureCount}});
      }
      case LibraryScanVerdict::Failed:
      {
        auto const error = outcome.optError ? std::string_view{outcome.optError->message} : std::string_view{};
        return requiredFormat(
          catalog, MessageId::LibraryScanFailed, {{"hasError", error.empty() ? "no" : "yes"}, {"error", error}});
      }
    }

    return requiredFormat(catalog, MessageId::LibraryScanFailed, {{"hasError", "no"}, {"error", ""}});
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

  std::string formatListMembershipMessage(MessageCatalog const& catalog,
                                          rt::AuthoringStatus const status,
                                          ListMembershipOperation const operation,
                                          std::string_view const listName,
                                          std::string_view const tagExpression,
                                          std::size_t const changedTrackCount,
                                          std::size_t const forgottenPositionCount)
  {
    if (operation == ListMembershipOperation::Add)
    {
      switch (status)
      {
        case rt::AuthoringStatus::Busy: return std::string{requiredText(catalog, MessageId::LibraryBusyTryAgain)};
        case rt::AuthoringStatus::Stale: return std::string{requiredText(catalog, MessageId::ListMembershipAddStale)};
        case rt::AuthoringStatus::Unavailable:
          return std::string{requiredText(catalog, MessageId::ListMembershipAddUnavailable)};
        case rt::AuthoringStatus::NoOp:
          return requiredFormat(
            catalog, MessageId::ListMembershipAddNoOp, {{"list", listName}, {"tag", tagExpression}});
        case rt::AuthoringStatus::Applied:
          return requiredFormat(catalog,
                                MessageId::ListMembershipAdded,
                                {{"tag", tagExpression}, {"count", changedTrackCount}, {"list", listName}});
      }
    }

    switch (status)
    {
      case rt::AuthoringStatus::Busy: return std::string{requiredText(catalog, MessageId::LibraryBusyTryAgain)};
      case rt::AuthoringStatus::Stale: return std::string{requiredText(catalog, MessageId::ListMembershipRemoveStale)};
      case rt::AuthoringStatus::Unavailable:
        return std::string{requiredText(catalog, MessageId::ListMembershipRemoveUnavailable)};
      case rt::AuthoringStatus::NoOp:
        return requiredFormat(
          catalog, MessageId::ListMembershipRemoveNoOp, {{"tag", tagExpression}, {"list", listName}});
      case rt::AuthoringStatus::Applied:
      {
        if (forgottenPositionCount == 0)
        {
          return requiredFormat(catalog,
                                MessageId::ListMembershipRemovedWithoutPosition,
                                {{"tag", tagExpression}, {"count", changedTrackCount}, {"list", listName}});
        }

        return requiredFormat(catalog,
                              MessageId::ListMembershipRemovedWithPositions,
                              {{"tag", tagExpression},
                               {"trackCount", changedTrackCount},
                               {"positionCount", forgottenPositionCount},
                               {"list", listName}});
      }
    }

    return {};
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

  std::string_view transportControlLabel(MessageCatalog const& catalog, PlaybackCommand const command) noexcept
  {
    switch (command)
    {
      case PlaybackCommand::Play:
      case PlaybackCommand::PlayPause: return requiredText(catalog, MessageId::PlaybackControlPlay);
      case PlaybackCommand::Pause: return requiredText(catalog, MessageId::PlaybackControlPause);
      case PlaybackCommand::Stop: return requiredText(catalog, MessageId::PlaybackControlStop);
      case PlaybackCommand::Next: return requiredText(catalog, MessageId::PlaybackControlNextTrack);
      case PlaybackCommand::Previous: return requiredText(catalog, MessageId::PlaybackControlPreviousTrack);
      case PlaybackCommand::ToggleShuffle: return requiredText(catalog, MessageId::PlaybackControlShuffle);
      case PlaybackCommand::CycleRepeat: return requiredText(catalog, MessageId::PlaybackControlRepeat);
    }

    return {};
  }

  std::string_view playbackActionLabel(MessageCatalog const& catalog, PlaybackCommand const command) noexcept
  {
    switch (command)
    {
      case PlaybackCommand::Play: return requiredText(catalog, MessageId::PlaybackControlPlay);
      case PlaybackCommand::Pause: return requiredText(catalog, MessageId::PlaybackControlPause);
      case PlaybackCommand::PlayPause: return requiredText(catalog, MessageId::PlaybackActionPlayPause);
      case PlaybackCommand::Stop: return requiredText(catalog, MessageId::PlaybackControlStop);
      case PlaybackCommand::Next: return requiredText(catalog, MessageId::PlaybackActionNext);
      case PlaybackCommand::Previous: return requiredText(catalog, MessageId::PlaybackActionPrevious);
      case PlaybackCommand::ToggleShuffle: return requiredText(catalog, MessageId::PlaybackActionToggleShuffle);
      case PlaybackCommand::CycleRepeat: return requiredText(catalog, MessageId::PlaybackActionCycleRepeat);
    }

    return {};
  }

  std::string volumeTooltip(MessageCatalog const& catalog,
                            std::int32_t const percent,
                            bool const muted,
                            bool const hardwareAssisted)
  {
    auto state = std::string_view{"other"};

    if (muted)
    {
      state = "muted";
    }
    else if (hardwareAssisted)
    {
      state = "hardware";
    }

    return requiredFormat(catalog, MessageId::PlaybackVolumeTooltip, {{"percent", percent}, {"state", state}});
  }
} // namespace ao::uimodel
