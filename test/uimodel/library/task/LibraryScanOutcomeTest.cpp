// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include "app/uimodel/library/task/LibraryScanWorkflow.h"
#include "test/MessageCatalogTestSupport.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/uimodel/status/activity/ActivityPresentationText.h>
#include <ao/utility/UnicodeText.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <string>
#include <utility>

namespace ao::uimodel::test
{
  namespace
  {
    using ScanResult = std::expected<LibraryScanWorkflowResult, LibraryScanWorkflowFailure>;

    ScanResult applied(rt::ScanApplyResult result)
    {
      auto workflow = LibraryScanWorkflowResult{};
      workflow.disposition = LibraryScanPlanDisposition::Actionable;
      workflow.summary.newCount = 1;
      workflow.optApplyResult = std::move(result);
      return workflow;
    }

    std::string messageFor(ScanResult const& result)
    {
      return formatLibraryScanMessage(ao::test::englishMessageCatalog(), decideLibraryScanOutcome(result));
    }
  } // namespace

  TEST_CASE("LibraryScanOutcome - a library already matching disk is reported quietly",
            "[uimodel][unit][library][scan]")
  {
    auto const result = ScanResult{LibraryScanWorkflowResult{}};
    auto const outcome = decideLibraryScanOutcome(result);

    CHECK(outcome.verdict == LibraryScanVerdict::UpToDate);
    CHECK(libraryScanSeverity(outcome.verdict) == rt::NotificationSeverity::Info);
    CHECK(libraryScanLifetime(outcome.verdict).kind() == rt::NotificationLifetimeKind::Transient);
    CHECK(messageFor(result) == "Library is up to date");
  }

  TEST_CASE("LibraryScanOutcome - a clean apply is reported quietly", "[uimodel][unit][library][scan]")
  {
    auto result = rt::ScanApplyResult{};
    result.insertedIds.emplace_back(1);

    auto const outcome = decideLibraryScanOutcome(applied(result));

    CHECK(outcome.verdict == LibraryScanVerdict::Complete);
    CHECK(libraryScanSeverity(outcome.verdict) == rt::NotificationSeverity::Info);
    CHECK(messageFor(applied(result)) == "Library scan complete");
  }

  TEST_CASE("LibraryScanOutcome - stale items left for a later scan are not errors", "[uimodel][unit][library][scan]")
  {
    auto result = rt::ScanApplyResult{};
    result.staleCount = 2;

    auto const outcome = decideLibraryScanOutcome(applied(result));

    CHECK(outcome.verdict == LibraryScanVerdict::Complete);
    CHECK(outcome.staleCount == 2);
    CHECK(outcome.failureCount == 0);
    CHECK(libraryScanSeverity(outcome.verdict) == rt::NotificationSeverity::Info);
    CHECK(libraryScanLifetime(outcome.verdict).kind() == rt::NotificationLifetimeKind::Transient);
  }

  TEST_CASE("LibraryScanOutcome - relinking a moved file is news, not a warning", "[uimodel][unit][library][scan]")
  {
    // Nothing was lost, so nobody has to do anything; saying so is a courtesy.
    auto result = rt::ScanApplyResult{};
    result.relinkedFileCount = 1;
    result.relinkedIds.emplace_back(1);

    CHECK(decideLibraryScanOutcome(applied(result)).verdict == LibraryScanVerdict::Complete);
    CHECK(decideLibraryScanOutcome(applied(result)).relinkedCount == 1);
    CHECK(messageFor(applied(result)) == "Relinked 1 moved file");

    result.relinkedFileCount = 2;
    result.relinkedIds.emplace_back(2);
    CHECK(decideLibraryScanOutcome(applied(result)).relinkedCount == 2);
    CHECK(messageFor(applied(result)) == "Relinked 2 moved files");
  }

  TEST_CASE("LibraryScanOutcome - two relinked member ids report one physical file", "[uimodel][unit][library][scan]")
  {
    // Member ids stay complete for library refresh. The reported count is the
    // physical file, so a two-member CUE carrier must not pluralize as two files.
    auto result = rt::ScanApplyResult{};
    result.relinkedFileCount = 1;
    result.relinkedIds.emplace_back(11);
    result.relinkedIds.emplace_back(12);

    auto const outcome = decideLibraryScanOutcome(applied(result));

    CHECK(outcome.verdict == LibraryScanVerdict::Complete);
    CHECK(outcome.relinkedCount == 1);
    CHECK(result.relinkedIds.size() == 2);
    CHECK(messageFor(applied(result)) == "Relinked 1 moved file");
  }

  TEST_CASE("LibraryScanOutcome - files the library lost need a person", "[uimodel][unit][library][scan]")
  {
    auto result = rt::ScanApplyResult{};
    result.missingCount = 1;

    auto const outcome = decideLibraryScanOutcome(applied(result));

    CHECK(outcome.verdict == LibraryScanVerdict::NeedsReview);
    CHECK(outcome.missingCount == 1);
    CHECK(libraryScanSeverity(outcome.verdict) == rt::NotificationSeverity::Warning);
    // Warnings survive the toast, because whoever has to act on one may not
    // have been looking when it appeared.
    CHECK(libraryScanLifetime(outcome.verdict).kind() == rt::NotificationLifetimeKind::History);
    CHECK(messageFor(applied(result)) == "1 missing file needs review");

    result.missingCount = 3;
    CHECK(messageFor(applied(result)) == "3 missing files need review");

    result.relinkedFileCount = 1;
    result.relinkedIds.emplace_back(1);
    CHECK(messageFor(applied(result)) == "Relinked 1 moved file; 3 missing files need review");
  }

  TEST_CASE("LibraryScanOutcome - notices on an applied plan need review but are not failures",
            "[uimodel][unit][library][scan]")
  {
    auto workflow = applied(rt::ScanApplyResult{});
    workflow->summary.newCount = 1;
    workflow->summary.noticeCount = 1;
    workflow->issues.push_back({.uri = "image.cue",
                                .message = "No supported audio file is named by this sheet",
                                .cueDiagnostic = rt::ScanCueDiagnostic::NoMatchingAudio,
                                .isNotice = true});

    auto const outcome = decideLibraryScanOutcome(workflow);

    CHECK(outcome.verdict == LibraryScanVerdict::NeedsReview);
    CHECK(outcome.failureCount == 0);
    CHECK(libraryScanLifetime(outcome.verdict).kind() == rt::NotificationLifetimeKind::History);
    CHECK(formatLibraryScanMessage(ao::test::englishMessageCatalog(), outcome)
            .starts_with("Library scan complete\nimage.cue: "));

    // A failure still outranks a notice.
    workflow->optApplyResult->failureCount = 1;
    CHECK(decideLibraryScanOutcome(workflow).verdict == LibraryScanVerdict::CompletedWithErrors);
  }

  TEST_CASE("LibraryScanOutcome - files that could not be applied outrank the rest", "[uimodel][unit][library][scan]")
  {
    auto result = rt::ScanApplyResult{};
    result.failureCount = 2;

    CHECK(decideLibraryScanOutcome(applied(result)).verdict == LibraryScanVerdict::CompletedWithErrors);
    CHECK(messageFor(applied(result)) == "Scan completed with errors");

    result.missingCount = 4;
    CHECK(decideLibraryScanOutcome(applied(result)).verdict == LibraryScanVerdict::CompletedWithErrors);
    CHECK(messageFor(applied(result)) == "Scan completed with errors; 4 missing files need review");
  }

  TEST_CASE("LibraryScanOutcome - a plan of nothing but unreadable files says how many",
            "[uimodel][unit][library][scan]")
  {
    auto workflow = LibraryScanWorkflowResult{};
    workflow.disposition = LibraryScanPlanDisposition::ErrorsOnly;
    workflow.summary.errorCount = 1;
    workflow.issues.push_back({.uri = "file:///broken.flac", .message = "Unsupported"});

    auto const outcome = decideLibraryScanOutcome(ScanResult{workflow});

    CHECK(outcome.verdict == LibraryScanVerdict::Unreadable);
    CHECK(outcome.failureCount == 1);
    CHECK(libraryScanSeverity(outcome.verdict) == rt::NotificationSeverity::Error);
    CHECK(messageFor(ScanResult{workflow}) == "Library scan found 1 unreadable file and no usable changes");

    workflow.summary.errorCount = 7;
    CHECK(messageFor(ScanResult{workflow}) == "Library scan found 7 unreadable files and no usable changes");
  }

  TEST_CASE("LibraryScanOutcome - a scan that did not finish carries its reason", "[uimodel][unit][library][scan]")
  {
    auto const result = ScanResult{std::unexpected{LibraryScanWorkflowFailure{
      .stage = LibraryScanWorkflowStage::Applying,
      .error = Error{.code = Error::Code::IoError, .message = "The library database is locked"},
    }}};

    auto const outcome = decideLibraryScanOutcome(result);

    CHECK(outcome.verdict == LibraryScanVerdict::Failed);
    REQUIRE(outcome.optError);
    CHECK(outcome.optError->code == Error::Code::IoError);
    CHECK(messageFor(result) == "Scan failed: The library database is locked");
  }

  TEST_CASE("LibraryScanOutcome - an actionable plan with no result is a failure, not a success",
            "[uimodel][unit][library][scan]")
  {
    // The workflow promises an applied plan or a failure. Neither means the two
    // halves disagree, and reporting that as a completed scan would hide it.
    auto workflow = LibraryScanWorkflowResult{};
    workflow.disposition = LibraryScanPlanDisposition::Actionable;
    workflow.summary.newCount = 1;

    auto const outcome = decideLibraryScanOutcome(ScanResult{workflow});

    CHECK(outcome.verdict == LibraryScanVerdict::Failed);
    REQUIRE(outcome.optError);
    CHECK(outcome.optError->code == Error::Code::InvalidState);
  }

  TEST_CASE("LibraryScanOutcome - deferred audio identity work survives the decision", "[uimodel][unit][library][scan]")
  {
    // A fast bootstrap scan leaves identity indexing for afterwards. A shell
    // that drops this leaves the library permanently half-indexed, so the
    // outcome carries it rather than the shell re-deriving the scan mode.
    auto workflow = LibraryScanWorkflowResult{};
    workflow.disposition = LibraryScanPlanDisposition::Actionable;
    workflow.summary.newCount = 1;
    workflow.optApplyResult = rt::ScanApplyResult{};
    workflow.shouldBackfillAudioIdentity = true;

    CHECK(decideLibraryScanOutcome(ScanResult{workflow}).shouldBackfillAudioIdentity);
  }

  TEST_CASE("LibraryScanOutcome - CUE notices survive reduction and display localized actionable reasons",
            "[uimodel][unit][library][scan]")
  {
    auto workflow = LibraryScanWorkflowResult{};
    workflow.disposition = LibraryScanPlanDisposition::UpToDate;
    workflow.summary.noticeCount = 1;
    workflow.issues.push_back({.uri = "音乐/image.flac",
                               .message = "English runtime wording must not choose a translation",
                               .cueDiagnostic = rt::ScanCueDiagnostic::WholeFileAlreadyImported,
                               .isNotice = true});
    auto const outcome = decideLibraryScanOutcome(ScanResult{workflow});
    REQUIRE(outcome.issues == workflow.issues);
    // A notice asks for review; it is not a failure.
    CHECK(outcome.verdict == LibraryScanVerdict::NeedsReview);
    CHECK(outcome.failureCount == 0);
    CHECK(libraryScanSeverity(outcome.verdict) == rt::NotificationSeverity::Warning);
    // Nothing was applied, so the library is still current.
    CHECK(formatLibraryScanMessage(ao::test::englishMessageCatalog(), outcome)
            .starts_with("Library is up to date\n音乐/image.flac: "));
    auto catalogRes = i18n::MessageCatalog::create("zh-Hans");
    REQUIRE(catalogRes);
    auto const message = formatLibraryScanMessage(*catalogRes, outcome);
    CHECK(message.contains("音乐/image.flac"));
    CHECK(message.contains("请移除该音轨后重新扫描以分割文件"));
    CHECK_FALSE(message.contains("English runtime wording"));
  }

  TEST_CASE("LibraryScanOutcome - CUE reasons retain external diagnostics but not unrelated raw errors",
            "[uimodel][unit][library][scan]")
  {
    auto outcome = LibraryScanOutcome{.verdict = LibraryScanVerdict::Unreadable};
    outcome.issues = {
      {.uri = "broken.cue",
       .message = "line 7: missing INDEX 01",
       .cueDiagnostic = rt::ScanCueDiagnostic::SheetUnreadableOrMalformed},
      {.uri = "image.flac",
       .message = "Competing CUE sheets:",
       .cueDiagnostic = rt::ScanCueDiagnostic::AmbiguousSheets,
       .detail = "image.flac.cue, image.FLAC.cue"},
      {.uri = "raw.flac", .message = "not a CUE diagnostic"},
    };
    auto const message = formatLibraryScanMessage(ao::test::englishMessageCatalog(), outcome);
    CHECK(message.contains("broken.cue: The CUE sheet could not be read or is malformed"));
    CHECK(message.contains("Details: line 7: missing INDEX 01"));
    CHECK(message.contains("Details: image.flac.cue, image.FLAC.cue"));
    CHECK_FALSE(message.contains("Competing CUE sheets:"));
    CHECK_FALSE(message.contains("not a CUE diagnostic"));
  }

  TEST_CASE("LibraryScanOutcome - invalid CUE layouts retain details without claiming segments were imported",
            "[uimodel][unit][library][scan]")
  {
    bool isNotice = false;

    SECTION("A new file has no imported Tracks")
    {
    }

    SECTION("An existing file keeps its stored form")
    {
      isNotice = true;
    }

    auto outcome = LibraryScanOutcome{
      .verdict = isNotice ? LibraryScanVerdict::NeedsReview : LibraryScanVerdict::Unreadable,
    };
    outcome.issues.push_back({.uri = "image.flac",
                              .message = "CUE TRACK 2 has an empty, overlapping, or out-of-extent range",
                              .cueDiagnostic = rt::ScanCueDiagnostic::LayoutMismatch,
                              .isNotice = isNotice});

    auto const message = formatLibraryScanMessage(ao::test::englishMessageCatalog(), outcome);
    CHECK(message.contains("The CUE layout is invalid or differs from imported ranges"));
    CHECK(message.contains("Check the sheet and scan again"));
    CHECK(message.contains("To replace existing ranges, first remove the imported Tracks"));
    CHECK_FALSE(message.contains("Segments were kept"));

    for (auto const* const locale : {"en", "de", "es", "fr", "ja", "zh-Hans", "zh-Hant"})
    {
      auto catalogRes = i18n::MessageCatalog::create(locale);
      REQUIRE(catalogRes);
      auto const localized = formatLibraryScanMessage(*catalogRes, outcome);
      CHECK(localized.contains("image.flac"));
      CHECK(localized.contains(outcome.issues.front().message));
    }
  }

  TEST_CASE("LibraryScanOutcome - every typed CUE reason is visible in every maintained locale",
            "[uimodel][unit][library][scan]")
  {
    auto const kinds = std::array{
      rt::ScanCueDiagnostic::InvalidEncoding,
      rt::ScanCueDiagnostic::SheetUnreadableOrMalformed,
      rt::ScanCueDiagnostic::AmbiguousSheets,
      rt::ScanCueDiagnostic::UnsupportedCarrier,
      rt::ScanCueDiagnostic::NoMatchingAudio,
      rt::ScanCueDiagnostic::WholeFileAlreadyImported,
      rt::ScanCueDiagnostic::SheetMissingForCarrier,
      rt::ScanCueDiagnostic::LayoutMismatch,
      rt::ScanCueDiagnostic::CarrierFactsMismatch,
    };

    for (auto const* const locale : {"en", "de", "es", "fr", "ja", "zh-Hans", "zh-Hant"})
    {
      auto catalogRes = i18n::MessageCatalog::create(locale);
      REQUIRE(catalogRes);

      for (auto const kind : kinds)
      {
        auto outcome = LibraryScanOutcome{.verdict = LibraryScanVerdict::Unreadable};
        outcome.issues.push_back({.uri = "外部/image.flac", .cueDiagnostic = kind});
        auto const message = formatLibraryScanMessage(*catalogRes, outcome);
        CHECK(message.contains("外部/image.flac"));
      }
    }
  }

  TEST_CASE("LibraryScanOutcome - CUE summaries show five issues and retain the omitted count",
            "[uimodel][unit][library][scan]")
  {
    auto outcome = LibraryScanOutcome{.verdict = LibraryScanVerdict::Unreadable, .failureCount = 7};
    outcome.issues.push_back({.uri = "ordinary.flac", .message = "not a CUE diagnostic"});

    for (std::size_t index = 0; index < 7; ++index)
    {
      outcome.issues.push_back(
        {.uri = "image-" + std::to_string(index) + ".cue", .cueDiagnostic = rt::ScanCueDiagnostic::NoMatchingAudio});
    }

    auto const issues = outcome.issues;
    auto const message = formatLibraryScanMessage(ao::test::englishMessageCatalog(), outcome);
    CHECK(message.starts_with("Library scan found 7 unreadable files and no usable changes"));
    CHECK(message.contains("image-4.cue"));
    CHECK_FALSE(message.contains("image-5.cue"));
    CHECK_FALSE(message.contains("ordinary.flac"));
    CHECK(message.ends_with("2 additional CUE issues; see the log for details."));
    CHECK(message.size() <= rt::NotificationFeedLimits::kDefaultMaxTextBytes);
    CHECK(outcome.issues == issues);
  }

  TEST_CASE("LibraryScanOutcome - oversized CUE values are omitted whole within every locale budget",
            "[uimodel][unit][library][scan]")
  {
    auto issue = LibraryScanIssue{.uri = "broken.cue",
                                  .message = "parser detail",
                                  .cueDiagnostic = rt::ScanCueDiagnostic::SheetUnreadableOrMalformed};

    SECTION("A long UTF-8 path cannot discard the notification")
    {
      issue.uri = std::string(rt::NotificationFeedLimits::kDefaultMaxTextBytes, 'x') + "/音乐.cue";
    }

    SECTION("A long UTF-8 parser detail cannot discard the notification")
    {
      for (std::size_t index = 0; index < 2000; ++index)
      {
        issue.message += "详情";
      }
    }

    auto outcome = LibraryScanOutcome{.verdict = LibraryScanVerdict::Unreadable, .failureCount = 2};
    outcome.issues = {issue, {.uri = "next.cue", .cueDiagnostic = rt::ScanCueDiagnostic::NoMatchingAudio}};

    for (auto const* locale : {"en", "de", "es", "fr", "ja", "zh-Hans", "zh-Hant"})
    {
      auto catalogRes = i18n::MessageCatalog::create(locale);
      REQUIRE(catalogRes);
      auto const message = formatLibraryScanMessage(*catalogRes, outcome);
      auto const suffix =
        i18n::requiredFormat(*catalogRes, i18n::MessageId::LibraryScanCueAdditionalIssues, {{"count", 2}});
      CHECK(message.size() <= rt::NotificationFeedLimits::kDefaultMaxTextBytes);
      CHECK(utility::validateUtf8(message));
      CHECK(message.ends_with(suffix));
      CHECK_FALSE(message.contains(issue.uri));
      CHECK_FALSE(message.contains("next.cue"));
    }

    CHECK(outcome.issues.front() == issue);
    CHECK(outcome.issues.size() == 2);
  }

  TEST_CASE("LibraryScanOutcome - long failed verdicts remain UTF-8 within the notification budget",
            "[uimodel][unit][library][scan]")
  {
    auto error = std::string{};

    for (std::size_t index = 0; index < 2000; ++index)
    {
      error += "失败";
    }

    auto outcome = LibraryScanOutcome{
      .verdict = LibraryScanVerdict::Failed,
      .optError = Error{.code = Error::Code::IoError, .message = error},
    };

    SECTION("The failed verdict has no CUE issues")
    {
      CHECK(outcome.issues.empty());
    }

    SECTION("The failed verdict reserves the omitted CUE count")
    {
      outcome.issues.push_back({.uri = "image.cue", .cueDiagnostic = rt::ScanCueDiagnostic::NoMatchingAudio});
    }

    auto const message = formatLibraryScanMessage(ao::test::englishMessageCatalog(), outcome);
    CHECK(message.starts_with("Scan failed:"));
    CHECK(message.size() <= rt::NotificationFeedLimits::kDefaultMaxTextBytes);
    CHECK(utility::validateUtf8(message));
    REQUIRE(outcome.optError);
    CHECK(outcome.optError->message == error);

    if (!outcome.issues.empty())
    {
      CHECK(message.ends_with("1 additional CUE issue; see the log for details."));
    }
  }

  TEST_CASE("LibraryScanOutcome - every verdict is said out loud", "[uimodel][unit][library][scan]")
  {
    // A verdict with no sentence would reach a notification as empty text.
    for (auto const verdict : {LibraryScanVerdict::UpToDate,
                               LibraryScanVerdict::Complete,
                               LibraryScanVerdict::NeedsReview,
                               LibraryScanVerdict::CompletedWithErrors,
                               LibraryScanVerdict::Unreadable,
                               LibraryScanVerdict::Failed})
    {
      CHECK_FALSE(formatLibraryScanMessage(ao::test::englishMessageCatalog(), {.verdict = verdict}).empty());
    }
  }
} // namespace ao::uimodel::test
