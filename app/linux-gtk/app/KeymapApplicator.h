// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace Gtk
{
  class Application;
}

namespace ao::uimodel::input
{
  class KeymapModel;
}

namespace ao::gtk
{
  /**
   * @brief Installs the effective keyboard map as GTK application accelerators.
   *
   * Each binding's chords are translated to GTK accelerator strings and applied via
   * Gtk::Application::set_accels_for_action(). Layout actions are exported to the window
   * ActionMap, so accelerators target the `win.` action prefix. Unmappable chords are
   * skipped with a warning. Calling this again fully replaces the prior accelerators,
   * so it is safe to re-run after the user edits their shortcuts.
   */
  void applyKeymapAccelerators(Gtk::Application& app, uimodel::input::KeymapModel const& keymap);
} // namespace ao::gtk
