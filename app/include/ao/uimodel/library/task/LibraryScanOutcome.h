// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace ao::uimodel
{
  /// What a finished scan amounts to for the person who asked for it.
  enum class LibraryScanVerdict : std::uint8_t
  {
    /// The library already matched what is on disk.
    UpToDate,
    /// The scan completed without item failures. Items whose evidence became
    /// stale may have been left for a later scan.
    Complete,
    /// Applied, but files the library knows about are no longer where it left
    /// them, which only a person can settle.
    NeedsReview,
    /// Applied, and files were lost along the way.
    CompletedWithErrors,
    /// Nothing readable was found to apply.
    Unreadable,
    /// The scan did not finish.
    Failed,
  };

  /**
   * @brief A finished scan reduced to what every shell has to decide about it.
   *
   * A scan ends in one of a handful of situations, and which one it is decides
   * three things at once: how loudly to say so, whether the reader should still
   * be able to find the message later, and what the sentence says. Those are
   * one judgement, not three, and not a judgement any single shell owns: a scan
   * that lost twelve files is the same event whichever window reports it.
   */
  struct LibraryScanOutcome final
  {
    LibraryScanVerdict verdict = LibraryScanVerdict::UpToDate;
    LibraryScanPlanSummary summary{};
    std::size_t relinkedCount = 0;
    std::size_t missingCount = 0;
    std::size_t staleCount = 0;
    std::size_t failureCount = 0;
    /// Set only when @ref verdict is Failed.
    std::optional<Error> optError{};
    /// Whether the scan deferred audio identity work it expects to be finished.
    bool shouldBackfillAudioIdentity = false;
  };

  /// How loudly @p verdict should be said.
  rt::NotificationSeverity libraryScanSeverity(LibraryScanVerdict verdict) noexcept;

  /**
   * @brief How long a report of @p verdict should stay reachable.
   *
   * Anything a person may have to act on is kept; an outcome that only confirms
   * the library is as they left it passes.
   */
  rt::NotificationLifetime libraryScanLifetime(LibraryScanVerdict verdict) noexcept;

  /**
   * @brief What @p result means, and the diagnostics that go with it.
   *
   * Also writes the plan summary and any failure to the log, and the unreadable
   * files when nothing was applied. Reading a scan and recording what it saw is
   * one pass over the same data, and splitting them is how one shell ends up
   * with diagnostics the other silently lacks. An applied plan reports its own
   * failed items as it reaches them, so those are not repeated here.
   */
  LibraryScanOutcome decideLibraryScanOutcome(
    std::expected<LibraryScanWorkflowResult, LibraryScanWorkflowFailure> const& result);
} // namespace ao::uimodel
