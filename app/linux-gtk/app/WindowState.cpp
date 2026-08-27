// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/WindowState.h"

namespace ao::gtk
{
  void recordWindowGeometry(WindowState& state, WindowState const& snapshot) noexcept
  {
    state.maximized = snapshot.maximized;

    if (snapshot.maximized)
    {
      return;
    }

    if (snapshot.width > 0)
    {
      state.width = snapshot.width;
    }

    if (snapshot.height > 0)
    {
      state.height = snapshot.height;
    }
  }
} // namespace ao::gtk
