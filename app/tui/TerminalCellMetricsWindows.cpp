// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArt.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ao::tui
{
  double queryTerminalCellAspectRatio() noexcept
  {
    auto consoleFont = CONSOLE_FONT_INFOEX{};
    consoleFont.cbSize = sizeof(consoleFont);

    if (auto* const handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        handle != nullptr && handle != INVALID_HANDLE_VALUE &&
        ::GetCurrentConsoleFontEx(handle, FALSE, &consoleFont) != 0)
    {
      return acceptedCellAspectRatio(
        static_cast<double>(consoleFont.dwFontSize.X), static_cast<double>(consoleFont.dwFontSize.Y));
    }

    return kDefaultCellAspectRatio;
  }
} // namespace ao::tui
