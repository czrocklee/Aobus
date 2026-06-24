// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/input/KeyChord.h>

#include <gdkmm/enums.h>
#include <glib.h>

#include <optional>
#include <string>

namespace ao::gtk
{
  /**
   * @brief Translates a neutral KeyChord into a GTK accelerator string.
   *
   * The result is suitable for Gtk::Application::set_accels_for_action(). Returns
   * std::nullopt if the chord's key token cannot be mapped to a GDK keysym. This
   * is the sole place that knows GDK keysyms and the `<Primary>p` accel syntax.
   */
  std::optional<std::string> toGtkAccel(uimodel::input::KeyChord const& chord);

  /**
   * @brief Parses a GTK accelerator string back into a neutral KeyChord.
   *
   * Returns std::nullopt if the string does not parse to a usable key.
   */
  std::optional<uimodel::input::KeyChord> fromGtkAccel(std::string const& accel);

  /**
   * @brief Converts a live GDK key press (keyval + modifier state) into a neutral KeyChord.
   *
   * Intended for interactive shortcut capture from a Gtk::EventControllerKey. Returns
   * std::nullopt when @p keyval is a standalone modifier (so capture keeps waiting) or
   * when the key cannot be mapped to a neutral token. Only the accelerator-relevant
   * modifier bits (Ctrl/Shift/Alt/Super) are retained from @p state.
   */
  std::optional<uimodel::input::KeyChord> fromGtkKeyval(guint keyval, Gdk::ModifierType state);
} // namespace ao::gtk
