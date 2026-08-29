// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/WindowInput.h"

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gtkmm/editable.h>
#include <gtkmm/textview.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <optional>

namespace ao::gtk
{
  namespace
  {
    constexpr std::int32_t kMouseBackButton = 8;
    constexpr std::int32_t kMouseForwardButton = 9;

    bool hasTextEditingFocus(Gtk::Widget const* widget)
    {
      for (auto const* current = widget; current != nullptr; current = current->get_parent())
      {
        if (dynamic_cast<Gtk::Editable const*>(current) != nullptr ||
            dynamic_cast<Gtk::TextView const*>(current) != nullptr)
        {
          return true;
        }
      }

      return false;
    }

    bool hasShortcutModifier(Gdk::ModifierType const modifiers)
    {
      auto const shortcutModifiers = Gdk::ModifierType::SHIFT_MASK | Gdk::ModifierType::CONTROL_MASK |
                                     Gdk::ModifierType::ALT_MASK | Gdk::ModifierType::SUPER_MASK |
                                     Gdk::ModifierType::HYPER_MASK | Gdk::ModifierType::META_MASK;
      return static_cast<bool>(modifiers & shortcutModifiers);
    }
  } // namespace

  std::optional<WorkspaceNavigation> mouseButtonNavigation(std::int32_t const button)
  {
    switch (button)
    {
      case kMouseBackButton: return WorkspaceNavigation::Back;
      case kMouseForwardButton: return WorkspaceNavigation::Forward;
      default: return std::nullopt;
    }
  }

  bool shouldActivatePlaybackSpaceShortcut(std::uint32_t const keyval,
                                           Gdk::ModifierType const modifiers,
                                           Gtk::Widget const* const focusWidget)
  {
    if (keyval != GDK_KEY_space && keyval != GDK_KEY_KP_Space)
    {
      return false;
    }

    if (hasShortcutModifier(modifiers))
    {
      return false;
    }

    return !hasTextEditingFocus(focusWidget);
  }
} // namespace ao::gtk
