// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gdkmm/enums.h>

#include <cstdint>

namespace Gtk
{
  class Widget;
}

namespace ao::gtk
{
  bool shouldActivatePlaybackSpaceShortcut(std::uint32_t keyval,
                                           Gdk::ModifierType modifiers,
                                           Gtk::Widget const* focusWidget);
} // namespace ao::gtk
