// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/NotificationState.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>

namespace ao::rt
{
  class LibraryJobs;
}

namespace ao::uimodel
{
  enum class LibraryScanMode : std::uint8_t
  {
    Eager,
    FastBootstrap
  };

  struct LibraryScanPlanSummary final
  {
    std::size_t newCount = 0;
    std::size_t changedCount = 0;
    std::size_t movedCount = 0;
    std::size_t missingCount = 0;
    std::size_t errorCount = 0;

    friend bool operator==(LibraryScanPlanSummary const&, LibraryScanPlanSummary const&) = default;
  };

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

  /** Run a scan and reduce every internal planning/apply state to one shell-facing outcome. */
  async::Task<LibraryScanOutcome> runLibraryScan(rt::LibraryJobs* jobs,
                                                 LibraryScanMode mode,
                                                 std::stop_token stopToken = {});
} // namespace ao::uimodel
