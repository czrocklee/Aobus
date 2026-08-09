// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <functional>

namespace ao::winui
{
  enum class DestructiveLibraryRestartOutcome : std::uint8_t
  {
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
    /**
     * @brief Release the window, session, runtime, and application-state stores.
     *
     * One operation rather than two, because the order in which a window and its
     * session go away is already fixed by their owner's member declaration order.
     * Naming the steps separately here would be a second, hand-maintained copy of
     * a guarantee the type system already gives.
     */
    std::move_only_function<void()> releaseActiveGraph;
    std::move_only_function<Result<>()> launchSuccessor;
    std::move_only_function<void(Error const&)> reportLaunchFailure;
    std::move_only_function<void()> exitProcess;
  };

  /**
   * @brief Releases the current process graph, attempts the successor launch, and exits.
   *
   * The launch is attempted even when release reports an unexpected exception.
   * The parent is exiting either way, so a successor that never starts is the
   * only outcome the user cannot recover from. After that attempt, an escaping
   * release exception enters AO fatal handling rather than being laundered into
   * a recoverable launch result.
   *
   * This function never reconstructs or rolls back the released graph.
   */
  DestructiveLibraryRestartOutcome executeDestructiveLibraryRestart(
    DestructiveLibraryRestartOperations operations) noexcept;
} // namespace ao::winui
