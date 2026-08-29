// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/status/activity/ActivityPresentationText.h>

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::uimodel
{
  using i18n::MessageCatalog;
  using i18n::MessageId;
  using i18n::requiredFormat;
  using i18n::requiredText;

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
} // namespace ao::uimodel
