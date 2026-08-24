// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <cstdint>

namespace ao::winui
{
  enum class DestructiveLibraryRestartOutcome : std::uint8_t
  {
    PreparationFailed,
    Launched,
    LaunchFailed,
  };

  /**
   * @brief Operations supplied by the platform owner of a destructive restart.
   *
   * Launch failure is reported through `Result`. An exception escaping any
   * operation is not another recoverable launch channel; the enclosing restart
   * boundary diagnoses it before aborting. If active-graph release escapes, the
   * owner still attempts the successor first so the dying parent does not cost
   * the user the replacement process.
   */
  struct DestructiveLibraryRestartOperations final
  {
    /** Checkpoint and terminally retire persistence while the active graph remains usable on failure. */
    compat::MoveOnlyFunction<Result<>()> prepareActiveGraph;

    /**
     * @brief Release the window, session, runtime, and application-state stores.
     *
     * One operation rather than two, because the order in which a window and its
     * session go away is already fixed by their owner's member declaration order.
     * Naming the steps separately here would be a second, hand-maintained copy of
     * a guarantee the type system already gives.
     */
    compat::MoveOnlyFunction<void()> releaseActiveGraph;
    compat::MoveOnlyFunction<Result<>()> launchSuccessor;
    compat::MoveOnlyFunction<void(Error const&)> reportPreparationFailure;
    compat::MoveOnlyFunction<void(Error const&)> reportLaunchFailure;
    compat::MoveOnlyFunction<void()> exitProcess;
  };

  /**
   * @brief Prepares and releases the current graph, attempts the successor launch, and exits.
   *
   * The launch is attempted even when release reports an unexpected exception.
   * The parent is exiting either way, so a successor that never starts is the
   * only outcome the user cannot recover from. After that attempt, an escaping
   * release exception enters AO fatal handling rather than being laundered into
   * a recoverable launch result.
   *
   * A failed preparation keeps the graph live and returns without launch or exit.
   * After preparation succeeds, this function never reconstructs or rolls back the released graph.
   */
  DestructiveLibraryRestartOutcome executeDestructiveLibraryRestart(
    DestructiveLibraryRestartOperations operations) noexcept;
} // namespace ao::winui
