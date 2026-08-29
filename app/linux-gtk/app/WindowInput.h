// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gdkmm/enums.h>

#include <cstdint>
#include <optional>

namespace Gtk
{
  class Widget;
}

namespace ao::gtk
{
  enum class WorkspaceNavigation : std::uint8_t
  {
    Back,
    Forward,
  };

  /** Map conventional mouse thumb buttons to workspace history navigation. */
  std::optional<WorkspaceNavigation> mouseButtonNavigation(std::int32_t button);

  /** Preserve unmodified Space for text editors; otherwise admit the playback shortcut. */
  bool shouldActivatePlaybackSpaceShortcut(std::uint32_t keyval,
                                           Gdk::ModifierType modifiers,
                                           Gtk::Widget const* focusWidget);
} // namespace ao::gtk
