// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include <ao/Error.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <catch2/catch_test_macros.hpp>

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
      return ao::test::englishPresentationTextCatalog().libraryScanMessage(decideLibraryScanOutcome(result));
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

  TEST_CASE("LibraryScanOutcome - stale items left for a later scan are not errors",
            "[uimodel][regression][library][scan]")
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
    result.relinkedIds.emplace_back(1);

    CHECK(decideLibraryScanOutcome(applied(result)).verdict == LibraryScanVerdict::Complete);
    CHECK(messageFor(applied(result)) == "Relinked 1 moved file");

    result.relinkedIds.emplace_back(2);
    CHECK(messageFor(applied(result)) == "Relinked 2 moved files");
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

    result.relinkedIds.emplace_back(1);
    CHECK(messageFor(applied(result)) == "Relinked 1 moved file; 3 missing files need review");
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
      CHECK_FALSE(ao::test::englishPresentationTextCatalog().libraryScanMessage({.verdict = verdict}).empty());
    }
  }
} // namespace ao::uimodel::test
