// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::winui::layout
{
  /// Shell-lifetime visibility and minimization used to admit component UI work.
  struct WindowActivityState final
  {
    bool visible = true;
    bool minimized = false;

    friend bool operator==(WindowActivityState const&, WindowActivityState const&) = default;
  };
} // namespace ao::winui::layout
