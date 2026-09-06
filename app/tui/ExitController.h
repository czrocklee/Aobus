// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <functional>

namespace ao::tui
{
  /**
   * @brief Idempotent graceful-exit gate for the terminal shell.
   *
   * The first request writes its new @ref Phase before invoking @ref Outputs.
   * A nested request therefore observes the updated phase, and
   * @ref Outputs::postExit is emitted at most once. This type has no FTXUI,
   * screen, runtime, or editor dependency.
   *
   * A submitted metadata write is the one thing worth waiting for: the shell
   * asks for it once, at the first request, and then waits for a settlement
   * notification instead of polling or keeping a second pending ledger. There
   * is at most one such write, so there is no count and no registration API.
   */
  class ExitController final
  {
  public:
    enum class Phase : std::uint8_t
    {
      Running,
      WaitingForSubmittedWrite,
      ExitPosted,
    };

    struct Outputs final
    {
      std::function<void()> retire;
      std::function<void()> postExit;
      /// Whether a submitted metadata write is still settling; absent means none ever is.
      std::function<bool()> hasPendingSubmittedWrite{};
    };

    explicit ExitController(Outputs outputs);

    /**
     * @brief Asks to leave, from root Quit, `:quit`, Ctrl-C, or a platform signal.
     *
     * A second request while waiting stops the application-level wait. That is
     * not a hard kill: runtime shutdown may still need to stop and join work.
     */
    void requestExit();

    /// Reports that the submitted write settled; a no-op outside the waiting phase.
    void notifySubmittedWriteSettled();

    Phase phase() const noexcept { return _phase; }
    bool isWaitingForSubmittedWrite() const noexcept { return _phase == Phase::WaitingForSubmittedWrite; }

  private:
    void postExitOnce();

    Outputs _outputs;
    Phase _phase = Phase::Running;
    bool _exitPosted = false;
  };
} // namespace ao::tui
