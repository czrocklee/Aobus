// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ao::winui
{
  /**
   * @brief The warnings raised while a library opens, as the status line shows them.
   *
   * The Classic preset has no activity surface, so its status line is the only
   * place it reports them. The startup scan's busy status displaces them; they
   * return once that scan settles, unless it failed.
   */
  class StartupWarnings final
  {
  public:
    /// Records a warning and returns the status line that now shows every warning.
    std::string add(std::string warning);
    /// Every warning, in the order they arose, as one status line; empty when there are none.
    std::string statusLine() const;
    /**
     * Ends the warnings' claim on the status line. Returns the line that a
     * settled scan shows in place of its own status: the warnings, when the
     * session scans at open and that scan did not fail. Any later scan finds
     * none.
     */
    std::optional<std::string> settle(bool scansAtOpen, bool scanFailed);

  private:
    std::vector<std::string> _warnings;
  };
} // namespace ao::winui
