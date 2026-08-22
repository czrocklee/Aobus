// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArt.h"

#include <unistd.h>

#include <sys/ioctl.h>

namespace ao::tui
{
  double queryTerminalCellAspectRatio() noexcept
  {
    // ioctl is a C vararg entry point, and <sys/ioctl.h> is the portable header
    // for TIOCGWINSZ even where the macro itself is defined deeper.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,misc-include-cleaner)
    if (auto ws = winsize{}; ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
    {
      return acceptedCellAspectRatio(static_cast<double>(ws.ws_xpixel) / static_cast<double>(ws.ws_col),
                                     static_cast<double>(ws.ws_ypixel) / static_cast<double>(ws.ws_row));
    }

    return kDefaultCellAspectRatio;
  }
} // namespace ao::tui
