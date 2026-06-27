// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/PlaybackShortcutPolicy.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("PlaybackShortcutPolicy preserves space for text editing widgets", "[gtk][unit][app][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto box = Gtk::Box{};
    auto entry = Gtk::Entry{};
    auto textView = Gtk::TextView{};
    auto label = Gtk::Label{"Tracks"};

    box.append(entry);
    box.append(textView);
    box.append(label);
    window.set_child(box);

    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType{}, &entry));
    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_KP_Space, Gdk::ModifierType{}, &textView));

    CHECK(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType{}, &label));
    CHECK(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType{}, nullptr));

    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_Return, Gdk::ModifierType{}, &label));
    CHECK_FALSE(shouldActivatePlaybackSpaceShortcut(GDK_KEY_space, Gdk::ModifierType::CONTROL_MASK, &label));
  }
} // namespace ao::gtk::test
