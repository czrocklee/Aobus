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
   * The first request writes @ref Phase::ExitPosted before invoking
   * @ref Outputs. A nested request therefore observes the updated phase.
   * @ref Outputs::postExit is emitted at most once. This type has no FTXUI,
   * screen, runtime, or editor dependency.
   */
  class ExitController final
  {
  public:
    enum class Phase : std::uint8_t
    {
      Running,
      ExitPosted,
    };

    struct Outputs final
    {
      std::function<void()> retire;
      std::function<void()> postExit;
    };

    explicit ExitController(Outputs outputs);

    void requestExit();
    Phase phase() const noexcept { return _phase; }

  private:
    Outputs _outputs;
    Phase _phase = Phase::Running;
  };
} // namespace ao::tui
