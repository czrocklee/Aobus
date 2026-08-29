// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/WindowInput.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gtkmm/box.h>
#include <gtkmm/editable.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("WindowInput - thumb buttons map to history navigation", "[gtk][unit][app][navigation]")
  {
    CHECK(mouseButtonNavigation(8) == WorkspaceNavigation::Back);
    CHECK(mouseButtonNavigation(9) == WorkspaceNavigation::Forward);

    for (auto const button : {0, 1, 2, 3, 10})
    {
      CHECK_FALSE(mouseButtonNavigation(button).has_value());
    }
  }

  TEST_CASE("WindowInput - playback Space preserves text editing", "[gtk][unit][app][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto box = Gtk::Box{};
    auto entry = Gtk::Entry{};
    auto textView = Gtk::TextView{};
    auto label = Gtk::Label{"Tracks"};
    entry.set_icon_from_icon_name("system-search-symbolic", Gtk::Entry::IconPosition::PRIMARY);

    box.append(entry);
    box.append(textView);
    box.append(label);
    window.set_child(box);

    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType{}, &entry));
    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_KP_Space, Gdk::ModifierType{}, &textView));

    auto* nestedEntryChild = entry.get_first_child();

    while (nestedEntryChild != nullptr && dynamic_cast<Gtk::Editable*>(nestedEntryChild) != nullptr)
    {
      nestedEntryChild = nestedEntryChild->get_next_sibling();
    }

    REQUIRE(nestedEntryChild != nullptr);
    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType{}, nestedEntryChild));

    CHECK(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType{}, &label));
    CHECK(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType{}, nullptr));
    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_Return, Gdk::ModifierType{}, &label));
    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType::CONTROL_MASK, &label));
  }
} // namespace ao::gtk::test
