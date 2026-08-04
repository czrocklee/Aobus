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
   * Releasing the active graph and launching the successor may fail, so this
   * boundary contains and classifies them. Failure reporting and process-exit
   * adapters are terminal operations and must establish their own no-throw
   * guarantees at the platform calls they own.
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
    std::move_only_function<void(Error const&) noexcept> reportLaunchFailure;
    std::move_only_function<void() noexcept> exitProcess;
  };

  /**
   * @brief Releases the current process graph, attempts the successor launch, and exits.
   *
   * The launch is attempted even when the release fails. The parent is exiting
   * either way, so a successor that never starts is the only outcome the user
   * cannot recover from; a half-released dying parent is not. A release failure
   * is therefore contained and not reported - whatever the user needs to know
   * arrives as the launch failure, if the launch also fails.
   *
   * This function never reconstructs or rolls back the released graph.
   */
  DestructiveLibraryRestartOutcome executeDestructiveLibraryRestart(
    DestructiveLibraryRestartOperations operations) noexcept;
} // namespace ao::winui
