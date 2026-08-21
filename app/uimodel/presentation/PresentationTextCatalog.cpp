// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <ao/Contract.h>
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
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    using i18n::MessageArgument;
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

  struct PresentationTextCatalog::Impl final
  {
    explicit Impl(MessageCatalog const& value)
      : catalog{value}, audioQualityFormatter{value}
    {
    }

    MessageCatalog catalog;
    AudioQualityFormatter audioQualityFormatter;
  };

  PresentationTextCatalog::PresentationTextCatalog(MessageCatalog const& catalog)
    : _implPtr{std::make_shared<Impl>(catalog)}
  {
  }

  PresentationTextCatalog::~PresentationTextCatalog() = default;

  std::string_view PresentationTextCatalog::text(MessageId const id) const noexcept
  {
    auto result = _implPtr->catalog.text(id);

    if (!result)
    {
      AO_FATAL("Could not resolve required presentation message: {}", result.error().message);
    }

    return *result;
  }

  std::string PresentationTextCatalog::format(MessageId const id,
                                              std::initializer_list<MessageArgument> const arguments) const
  {
    auto result = _implPtr->catalog.format(id, arguments);

    if (!result)
    {
      AO_FATAL("Could not format required presentation message: {}", result.error().message);
    }

    return std::move(result->text);
  }

  std::string_view PresentationTextCatalog::trackFieldLabel(rt::TrackField const field) const noexcept
  {
    auto const index = static_cast<std::size_t>(field);
    return index < kTrackFieldMessageIds.size() ? text(kTrackFieldMessageIds[index]) : std::string_view{};
  }

  std::string_view PresentationTextCatalog::trackGroupKeyLabel(rt::TrackGroupKey const key) const noexcept
  {
    switch (key)
    {
      case rt::TrackGroupKey::None: return text(MessageId::TrackGroupNone);
      case rt::TrackGroupKey::Artist: return trackFieldLabel(rt::TrackField::Artist);
      case rt::TrackGroupKey::Album: return trackFieldLabel(rt::TrackField::Album);
      case rt::TrackGroupKey::AlbumArtist: return trackFieldLabel(rt::TrackField::AlbumArtist);
      case rt::TrackGroupKey::Genre: return trackFieldLabel(rt::TrackField::Genre);
      case rt::TrackGroupKey::Composer: return trackFieldLabel(rt::TrackField::Composer);
      case rt::TrackGroupKey::Conductor: return trackFieldLabel(rt::TrackField::Conductor);
      case rt::TrackGroupKey::Ensemble: return trackFieldLabel(rt::TrackField::Ensemble);
      case rt::TrackGroupKey::Work: return trackFieldLabel(rt::TrackField::Work);
      case rt::TrackGroupKey::Year: return trackFieldLabel(rt::TrackField::Year);
    }

    return {};
  }

  std::string_view PresentationTextCatalog::missingTrackValueLabel(rt::MissingTrackValueKind const kind) const noexcept
  {
    auto const index = static_cast<std::size_t>(kind);
    return index < kMissingTrackValueMessageIds.size() ? text(kMissingTrackValueMessageIds[index]) : std::string_view{};
  }

  std::optional<TrackPresentationText> PresentationTextCatalog::builtinTrackPresentation(
    std::string_view const id) const noexcept
  {
    for (auto const& definition : kBuiltinTrackPresentationDefinitions)
    {
      if (definition.id == id)
      {
        return TrackPresentationText{
          .label = text(definition.labelId),
          .description = text(definition.descriptionId),
        };
      }
    }

    return std::nullopt;
  }

  AudioBackendPresentation PresentationTextCatalog::audioBackend(audio::BackendId const& id) const
  {
    if (id == audio::kBackendPipeWire)
    {
      return AudioBackendPresentation{
        .label = "PipeWire",
        .description = std::string{text(MessageId::AudioBackendPipeWireDescription)},
        .shortLabel = "PW",
        .outputDeviceDescriptionFallback = "PipeWire",
        .iconKind = AudioIconKind::AudioServer,
      };
    }

    if (id == audio::kBackendAlsa)
    {
      return AudioBackendPresentation{
        .label = "ALSA",
        .description = std::string{text(MessageId::AudioBackendAlsaDescription)},
        .shortLabel = "ALSA",
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    if (id == audio::kBackendWasapi)
    {
      return AudioBackendPresentation{
        .label = "WASAPI",
        .description = std::string{text(MessageId::AudioBackendWasapiDescription)},
        .shortLabel = "WASAPI",
        .outputDeviceDescriptionFallback = std::string{text(MessageId::AudioBackendWasapiOutputFallback)},
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    auto const& fallback = id.raw();
    return AudioBackendPresentation{.label = std::string{fallback}, .shortLabel = std::string{fallback}};
  }

  AudioProfilePresentation PresentationTextCatalog::audioProfile(audio::ProfileId const& id) const
  {
    if (id == audio::kProfileShared)
    {
      return AudioProfilePresentation{
        .label = std::string{text(MessageId::AudioProfileShared)},
        .description = std::string{text(MessageId::AudioProfileSharedDescription)},
      };
    }

    if (id == audio::kProfileExclusive)
    {
      return AudioProfilePresentation{
        .label = std::string{text(MessageId::AudioProfileExclusive)},
        .description = std::string{text(MessageId::AudioProfileExclusiveDescription)},
      };
    }

    return AudioProfilePresentation{.label = std::string{id.raw()}};
  }

  std::string PresentationTextCatalog::completionDetail(rt::CompletionDetail const& detail) const
  {
    switch (detail.kind)
    {
      case rt::CompletionDetailKind::None: return {};
      case rt::CompletionDetailKind::ResolvedText: return detail.resolvedText;
      case rt::CompletionDetailKind::Field: return std::string{text(MessageId::CompletionField)};
      case rt::CompletionDetailKind::Alias: return std::string{text(MessageId::CompletionAlias)};
      case rt::CompletionDetailKind::Operator: return std::string{text(MessageId::CompletionOperator)};
      case rt::CompletionDetailKind::LogicalOperator: return std::string{text(MessageId::CompletionLogicalOperator)};
      case rt::CompletionDetailKind::Frequency: return std::to_string(detail.frequency);
    }

    return {};
  }

  std::string PresentationTextCatalog::notificationMessage(rt::NotificationMessage const& message) const
  {
    auto const* report = std::get_if<rt::NotificationReport>(&message);

    if (report == nullptr)
    {
      return std::get<std::string>(message);
    }

    auto const failureReason =
      report->detail.empty() ? std::string{text(MessageId::NotificationUnknownError)} : report->detail;
    auto const trackLabel = [&]
    {
      if (!report->subject.empty())
      {
        return report->subject;
      }

      if (report->trackId != kInvalidTrackId)
      {
        return format(MessageId::NotificationTrackLabel, {{"id", report->trackId.raw()}});
      }

      return std::string{text(MessageId::NotificationPlaybackSubject)};
    };

    switch (report->templateId)
    {
      case rt::NotificationReportTemplate::PlaybackTrackOpenFailed:
      {
        auto const track = trackLabel();
        return format(MessageId::NotificationTrackOpenFailed, {{"track", track}, {"reason", failureReason}});
      }
      case rt::NotificationReportTemplate::PlaybackDecodeFailed:
      {
        auto const track = trackLabel();
        return format(MessageId::NotificationDecodeFailed, {{"track", track}, {"reason", failureReason}});
      }
      case rt::NotificationReportTemplate::PlaybackRouteActivationFailed:
        return format(MessageId::NotificationRouteActivationFailed, {{"reason", failureReason}});
      case rt::NotificationReportTemplate::PlaybackDeviceLost:
        return format(MessageId::NotificationDeviceLost, {{"reason", failureReason}});
      case rt::NotificationReportTemplate::PlaybackSequenceFinished:
        return std::string{text(MessageId::NotificationSequenceFinished)};
      case rt::NotificationReportTemplate::PlaybackTracksSkipped:
        return format(MessageId::NotificationTracksSkipped, {{"count", report->count}});
      case rt::NotificationReportTemplate::PlaybackStoppedAfterFailures:
        return format(MessageId::NotificationStoppedAfterFailures, {{"count", report->count}});
      case rt::NotificationReportTemplate::PlaybackStoppedForTrack:
        return format(MessageId::NotificationStoppedForTrack,
                      {{"hasSubject", report->subject.empty() ? "no" : "yes"},
                       {"hasDetail", report->detail.empty() ? "no" : "yes"},
                       {"subject", report->subject},
                       {"detail", report->detail}});
    }

    return std::string{text(MessageId::NotificationFallback)};
  }

  std::string PresentationTextCatalog::notificationGroupMessage(rt::NotificationSeverity const severity,
                                                                std::size_t const count) const
  {
    auto messageId = MessageId::NotificationGroupedInfo;

    switch (severity)
    {
      case rt::NotificationSeverity::Info: messageId = MessageId::NotificationGroupedInfo; break;
      case rt::NotificationSeverity::Warning: messageId = MessageId::NotificationGroupedWarning; break;
      case rt::NotificationSeverity::Error: messageId = MessageId::NotificationGroupedError; break;
    }

    return format(messageId, {{"count", count}});
  }

  std::string PresentationTextCatalog::libraryTaskProgressDetail(rt::LibraryTaskProgressKind const kind,
                                                                 std::string_view const subject) const
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
    }

    auto const activity = text(activityId);
    return format(MessageId::LibraryTaskProgressDetail,
                  {{"hasSubject", subject.empty() ? "no" : "yes"}, {"activity", activity}, {"subject", subject}});
  }

  std::string PresentationTextCatalog::libraryTaskProgressCompact(rt::LibraryTaskProgressKind const kind,
                                                                  std::string_view const subject) const
  {
    switch (kind)
    {
      case rt::LibraryTaskProgressKind::Scanning: return std::string{text(MessageId::LibraryTaskScanningCompact)};
      case rt::LibraryTaskProgressKind::Updating: return std::string{text(MessageId::LibraryTaskUpdatingCompact)};
      case rt::LibraryTaskProgressKind::Fingerprinting:
      case rt::LibraryTaskProgressKind::IndexingAudioIdentity: return libraryTaskProgressDetail(kind, subject);
    }

    return {};
  }

  std::string PresentationTextCatalog::libraryScanMessage(LibraryScanOutcome const& outcome) const
  {
    auto const changeSummary = [&]
    {
      auto relinked = std::string{};

      if (outcome.relinkedCount > 0)
      {
        relinked = format(MessageId::LibraryScanRelinked, {{"count", outcome.relinkedCount}});
      }

      auto missing = std::string{};

      if (outcome.missingCount > 0)
      {
        missing = format(MessageId::LibraryScanMissing, {{"count", outcome.missingCount}});
      }

      if (!relinked.empty() && !missing.empty())
      {
        return format(MessageId::LibraryScanChangesCombined, {{"relinked", relinked}, {"missing", missing}});
      }

      if (!relinked.empty())
      {
        return relinked;
      }

      return missing;
    };

    switch (outcome.verdict)
    {
      case LibraryScanVerdict::UpToDate: return std::string{text(MessageId::LibraryScanUpToDate)};
      case LibraryScanVerdict::Complete:
      case LibraryScanVerdict::NeedsReview:
      {
        auto changes = changeSummary();
        return changes.empty() ? std::string{text(MessageId::LibraryScanComplete)} : std::move(changes);
      }
      case LibraryScanVerdict::CompletedWithErrors:
      {
        auto const changes = changeSummary();
        return format(MessageId::LibraryScanCompletedWithErrors,
                      {{"hasChanges", changes.empty() ? "no" : "yes"}, {"changes", changes}});
      }
      case LibraryScanVerdict::Unreadable:
      {
        return format(MessageId::LibraryScanUnreadable, {{"count", outcome.failureCount}});
      }
      case LibraryScanVerdict::Failed:
      {
        auto const error = outcome.optError ? std::string_view{outcome.optError->message} : std::string_view{};
        return format(MessageId::LibraryScanFailed, {{"hasError", error.empty() ? "no" : "yes"}, {"error", error}});
      }
    }

    return format(MessageId::LibraryScanFailed, {{"hasError", "no"}, {"error", ""}});
  }

  std::string PresentationTextCatalog::trackSelectionSummary(std::size_t const count,
                                                             std::string_view const duration) const
  {
    if (count == 0)
    {
      return {};
    }

    return format(MessageId::TrackSelectionSummary,
                  {{"count", count}, {"hasDuration", duration.empty() ? "no" : "yes"}, {"duration", duration}});
  }

  std::string PresentationTextCatalog::smartListMembershipEditingText(bool const direct,
                                                                      std::string_view const expression) const
  {
    if (!direct)
    {
      return std::string{text(MessageId::SmartListMembershipComputed)};
    }

    return format(MessageId::SmartListMembershipDirect, {{"expression", expression}});
  }

  std::string PresentationTextCatalog::smartListPreviewStatus(bool const expressionValid,
                                                              std::size_t const count,
                                                              bool const isAllTracks,
                                                              bool const localEmpty) const
  {
    if (localEmpty)
    {
      auto const source = isAllTracks ? std::string_view{"library"} : std::string_view{"source"};

      if (count == 0)
      {
        return format(MessageId::SmartListNoTracks, {{"source", source}});
      }

      return format(MessageId::SmartListShowingSource, {{"source", source}, {"count", count}});
    }

    if (!expressionValid)
    {
      return std::string{text(MessageId::SmartListInvalidFilter)};
    }

    if (count == 0)
    {
      return std::string{text(MessageId::SmartListNoMatches)};
    }

    constexpr std::size_t kMaxPreview = 10;

    if (count <= kMaxPreview)
    {
      return format(MessageId::SmartListShowingAllMatches, {{"count", count}});
    }

    return format(MessageId::SmartListShowingFirstMatches, {{"visible", kMaxPreview}, {"count", count}});
  }

  std::string PresentationTextCatalog::listMembershipNotification(rt::TrackAuthoringStatus const status,
                                                                  ListMembershipOperation const operation,
                                                                  std::string_view const listName,
                                                                  std::string_view const tagExpression,
                                                                  std::size_t const changedTrackCount,
                                                                  std::size_t const forgottenPositionCount) const
  {
    if (operation == ListMembershipOperation::Add)
    {
      switch (status)
      {
        case rt::TrackAuthoringStatus::Stale: return std::string{text(MessageId::ListMembershipAddStale)};
        case rt::TrackAuthoringStatus::Unavailable: return std::string{text(MessageId::ListMembershipAddUnavailable)};
        case rt::TrackAuthoringStatus::NoOp:
          return format(MessageId::ListMembershipAddNoOp, {{"list", listName}, {"tag", tagExpression}});
        case rt::TrackAuthoringStatus::Applied:
          return format(
            MessageId::ListMembershipAdded, {{"tag", tagExpression}, {"count", changedTrackCount}, {"list", listName}});
      }
    }

    switch (status)
    {
      case rt::TrackAuthoringStatus::Stale: return std::string{text(MessageId::ListMembershipRemoveStale)};
      case rt::TrackAuthoringStatus::Unavailable: return std::string{text(MessageId::ListMembershipRemoveUnavailable)};
      case rt::TrackAuthoringStatus::NoOp:
        return format(MessageId::ListMembershipRemoveNoOp, {{"tag", tagExpression}, {"list", listName}});
      case rt::TrackAuthoringStatus::Applied:
      {
        if (forgottenPositionCount == 0)
        {
          return format(MessageId::ListMembershipRemovedWithoutPosition,
                        {{"tag", tagExpression}, {"count", changedTrackCount}, {"list", listName}});
        }

        return format(MessageId::ListMembershipRemovedWithPositions,
                      {{"tag", tagExpression},
                       {"trackCount", changedTrackCount},
                       {"positionCount", forgottenPositionCount},
                       {"list", listName}});
      }
    }

    return {};
  }

  std::string PresentationTextCatalog::trackChannelText(std::uint8_t const channels) const
  {
    if (channels == 1)
    {
      return std::string{text(MessageId::TrackChannelMono)};
    }

    if (channels == 2)
    {
      return std::string{text(MessageId::TrackChannelStereo)};
    }

    return format(MessageId::TrackChannelCount, {{"count", channels}});
  }

  std::string_view PresentationTextCatalog::transportControlLabel(PlaybackCommand const command) const noexcept
  {
    switch (command)
    {
      case PlaybackCommand::Play:
      case PlaybackCommand::PlayPause: return text(MessageId::PlaybackControlPlay);
      case PlaybackCommand::Pause: return text(MessageId::PlaybackControlPause);
      case PlaybackCommand::Stop: return text(MessageId::PlaybackControlStop);
      case PlaybackCommand::Next: return text(MessageId::PlaybackControlNextTrack);
      case PlaybackCommand::Previous: return text(MessageId::PlaybackControlPreviousTrack);
      case PlaybackCommand::ToggleShuffle: return text(MessageId::PlaybackControlShuffle);
      case PlaybackCommand::CycleRepeat: return text(MessageId::PlaybackControlRepeat);
    }

    return {};
  }

  std::string_view PresentationTextCatalog::playbackActionLabel(PlaybackCommand const command) const noexcept
  {
    switch (command)
    {
      case PlaybackCommand::Play: return text(MessageId::PlaybackControlPlay);
      case PlaybackCommand::Pause: return text(MessageId::PlaybackControlPause);
      case PlaybackCommand::PlayPause: return text(MessageId::PlaybackActionPlayPause);
      case PlaybackCommand::Stop: return text(MessageId::PlaybackControlStop);
      case PlaybackCommand::Next: return text(MessageId::PlaybackActionNext);
      case PlaybackCommand::Previous: return text(MessageId::PlaybackActionPrevious);
      case PlaybackCommand::ToggleShuffle: return text(MessageId::PlaybackActionToggleShuffle);
      case PlaybackCommand::CycleRepeat: return text(MessageId::PlaybackActionCycleRepeat);
    }

    return {};
  }

  std::string PresentationTextCatalog::volumeTooltip(std::int32_t const percent,
                                                     bool const muted,
                                                     bool const hardwareAssisted) const
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

    return format(MessageId::PlaybackVolumeTooltip, {{"percent", percent}, {"state", state}});
  }

  AudioQualityFormatter const& PresentationTextCatalog::audioQualityFormatter() const noexcept
  {
    return _implPtr->audioQualityFormatter;
  }
} // namespace ao::uimodel
