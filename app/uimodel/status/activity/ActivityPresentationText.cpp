// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/status/activity/ActivityPresentationText.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/utility/UnicodeText.h>

#include <algorithm>
#include <cstddef>
#include <optional>
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

  namespace
  {
    constexpr std::size_t kMaxScanCueIssues = 5;
    constexpr auto kScanMessageBytes = rt::NotificationFeedLimits::kDefaultMaxTextBytes;

    MessageId completedScanMessageId(LibraryScanOutcome const& outcome)
    {
      if (auto const& summary = outcome.summary;
          summary.newCount != 0 || summary.changedCount != 0 || summary.movedCount != 0 || summary.missingCount != 0)
      {
        return MessageId::LibraryScanComplete;
      }

      return MessageId::LibraryScanUpToDate;
    }

    std::string scanChangeSummary(MessageCatalog const& catalog, LibraryScanOutcome const& outcome)
    {
      auto relinked = std::string{};

      if (outcome.relinkedCount > 0)
      {
        relinked = requiredFormat(catalog, MessageId::LibraryScanRelinked, {{"count", outcome.relinkedCount}});
      }

      if (outcome.missingCount == 0)
      {
        return relinked;
      }

      auto missing = requiredFormat(catalog, MessageId::LibraryScanMissing, {{"count", outcome.missingCount}});

      if (relinked.empty())
      {
        return missing;
      }

      return requiredFormat(
        catalog, MessageId::LibraryScanChangesCombined, {{"relinked", relinked}, {"missing", missing}});
    }

    void appendScanCueIssue(std::string& message, MessageCatalog const& catalog, LibraryScanIssue const& issue)
    {
      auto const optMessageId = [&] -> std::optional<MessageId>
      {
        switch (issue.cueDiagnostic)
        {
          case rt::ScanCueDiagnostic::None: return std::nullopt;
          case rt::ScanCueDiagnostic::InvalidEncoding: return MessageId::LibraryScanCueInvalidEncoding;
          case rt::ScanCueDiagnostic::SheetUnreadableOrMalformed: return MessageId::LibraryScanCueUnreadable;
          case rt::ScanCueDiagnostic::AmbiguousSheets: return MessageId::LibraryScanCueAmbiguous;
          case rt::ScanCueDiagnostic::UnsupportedCarrier: return MessageId::LibraryScanCueUnsupported;
          case rt::ScanCueDiagnostic::NoMatchingAudio: return MessageId::LibraryScanCueNoMatchingAudio;
          case rt::ScanCueDiagnostic::WholeFileAlreadyImported: return MessageId::LibraryScanCueWholeFile;
          case rt::ScanCueDiagnostic::SheetMissingForCarrier: return MessageId::LibraryScanCueMissing;
          case rt::ScanCueDiagnostic::LayoutMismatch: return MessageId::LibraryScanCueLayoutMismatch;
          case rt::ScanCueDiagnostic::CarrierFactsMismatch: return MessageId::LibraryScanCueCarrierMismatch;
        }

        return std::nullopt;
      }();

      if (!optMessageId)
      {
        return;
      }

      message += '\n';
      message += requiredFormat(catalog, *optMessageId, {{"uri", issue.uri}});

      if ((issue.cueDiagnostic == rt::ScanCueDiagnostic::SheetUnreadableOrMalformed ||
           issue.cueDiagnostic == rt::ScanCueDiagnostic::LayoutMismatch) &&
          !issue.message.empty())
      {
        message += requiredFormat(catalog, MessageId::LibraryScanCueDetail, {{"detail", issue.message}});
      }
      else if (issue.cueDiagnostic == rt::ScanCueDiagnostic::AmbiguousSheets && !issue.detail.empty())
      {
        message += requiredFormat(catalog, MessageId::LibraryScanCueDetail, {{"detail", issue.detail}});
      }
    }

    std::string scanCueOmission(MessageCatalog const& catalog, std::size_t count)
    {
      return count == 0 ? std::string{}
                        : '\n' + requiredFormat(catalog, MessageId::LibraryScanCueAdditionalIssues, {{"count", count}});
    }

    void boundScanVerdict(std::string& message, MessageCatalog const& catalog, std::size_t maxBytes)
    {
      if (message.size() <= maxBytes)
      {
        return;
      }

      auto prefixEndRes = utility::previousUtf8GraphemeBoundary(message, maxBytes);

      if (prefixEndRes && *prefixEndRes > 0)
      {
        message.resize(*prefixEndRes);
      }
      else
      {
        message = requiredFormat(catalog, MessageId::LibraryScanFailed, {{"hasError", "no"}, {"error", ""}});
      }
    }

    void appendScanIssueSummary(std::string& message,
                                MessageCatalog const& catalog,
                                LibraryScanOutcome const& outcome,
                                std::string_view logNotice)
    {
      auto remaining = static_cast<std::size_t>(std::ranges::count_if(
        outcome.issues,
        [](LibraryScanIssue const& issue) { return issue.cueDiagnostic != rt::ScanCueDiagnostic::None; }));
      auto const reservedBytes =
        scanCueOmission(catalog, remaining).size() + (logNotice.empty() ? 0 : 1 + logNotice.size());
      AO_INVARIANT(reservedBytes < kScanMessageBytes, "The scan issue summary must leave room for its verdict");
      boundScanVerdict(message, catalog, kScanMessageBytes - reservedBytes);

      if (!logNotice.empty())
      {
        message += '\n';
        message += logNotice;
      }

      std::size_t included = 0;

      for (auto const& issue : outcome.issues)
      {
        if (issue.cueDiagnostic == rt::ScanCueDiagnostic::None)
        {
          continue;
        }

        if (included == kMaxScanCueIssues)
        {
          break;
        }

        auto line = std::string{};
        appendScanCueIssue(line, catalog, issue);

        if (auto const tail = scanCueOmission(catalog, remaining - 1);
            message.size() + line.size() + tail.size() > kScanMessageBytes)
        {
          break;
        }

        message += line;
        ++included;
        --remaining;
      }

      message += scanCueOmission(catalog, remaining);
    }
  } // namespace

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
    auto const hasLogOnlyNotice =
      std::ranges::any_of(outcome.issues,
                          [](LibraryScanIssue const& issue)
                          { return issue.isNotice && issue.cueDiagnostic == rt::ScanCueDiagnostic::None; });
    auto logNotice =
      hasLogOnlyNotice ? requiredText(catalog, MessageId::LibraryScanNoticesNeedReview) : std::string_view{};

    auto message = [&] -> std::string
    {
      switch (outcome.verdict)
      {
        case LibraryScanVerdict::UpToDate: return std::string{requiredText(catalog, MessageId::LibraryScanUpToDate)};
        case LibraryScanVerdict::Complete:
        case LibraryScanVerdict::NeedsReview:
        {
          if (auto changes = scanChangeSummary(catalog, outcome); !changes.empty())
          {
            return changes;
          }

          auto const messageId = completedScanMessageId(outcome);

          if (messageId == MessageId::LibraryScanUpToDate && !logNotice.empty())
          {
            // Lead with the notice rather than appending it to an up-to-date verdict.
            return std::string{std::exchange(logNotice, {})};
          }

          return std::string{requiredText(catalog, messageId)};
        }
        case LibraryScanVerdict::CompletedWithErrors:
        {
          auto const changes = scanChangeSummary(catalog, outcome);
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
    }();

    appendScanIssueSummary(message, catalog, outcome, logNotice);
    return message;
  }

  rt::NotificationRequest librarySwitchNotSavedNotification(MessageCatalog const& catalog, Error const& error)
  {
    return rt::NotificationRequest{
      .severity = rt::NotificationSeverity::Warning,
      .message = requiredFormat(catalog, MessageId::LibrarySwitchNotSaved, {{"detail", error.message}}),
      .lifetime = rt::NotificationLifetime::history(),
    };
  }
} // namespace ao::uimodel
