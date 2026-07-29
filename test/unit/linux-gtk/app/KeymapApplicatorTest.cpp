// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/KeymapApplicator.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>

#include <string>

namespace ao::gtk::test
{
  namespace
  {
    uimodel::KeyChord chord(std::string const& text)
    {
      auto const optChord = uimodel::KeyChord::parse(text);
      REQUIRE(optChord);
      return *optChord;
    }
  }

  TEST_CASE("applyKeymapAccelerators installs accelerators for bound actions", "[gtk][unit][app][accel]")
  {
    auto const appPtr = ensureGtkApplication();
    Gtk::Application& application = *appPtr;

    auto model = uimodel::KeymapModel{uimodel::KeymapBindings{{"applicator.install", {chord("Ctrl+P")}}}};
    applyKeymapAccelerators(application, model);

    CHECK_FALSE(application.get_accels_for_action("win.applicator.install").empty());
  }

  TEST_CASE("applyKeymapAccelerators clears accelerators dropped from the keymap", "[gtk][unit][app][accel]")
  {
    auto const appPtr = ensureGtkApplication();
    Gtk::Application& application = *appPtr;

    // Bind an action whose id is absent from the default keymap, then re-apply a keymap that no
    // longer mentions it (mirrors resetToDefault on a binding with no shipped default, which erases
    // the entry). The stale accelerator must not survive the re-apply.
    auto bound = uimodel::KeymapModel{uimodel::KeymapBindings{{"applicator.drop", {chord("Ctrl+J")}}}};
    applyKeymapAccelerators(application, bound);
    REQUIRE_FALSE(application.get_accels_for_action("win.applicator.drop").empty());

    auto dropped = uimodel::KeymapModel{uimodel::KeymapBindings{}};
    applyKeymapAccelerators(application, dropped);

    CHECK(application.get_accels_for_action("win.applicator.drop").empty());
  }

  TEST_CASE("applyKeymapAccelerators clears an explicitly unbound action", "[gtk][unit][app][accel]")
  {
    auto const appPtr = ensureGtkApplication();
    Gtk::Application& application = *appPtr;

    auto bound = uimodel::KeymapModel{uimodel::KeymapBindings{{"applicator.unbind", {chord("Ctrl+K")}}}};
    applyKeymapAccelerators(application, bound);
    REQUIRE_FALSE(application.get_accels_for_action("win.applicator.unbind").empty());

    // An empty chord list (still present in the keymap) means "explicitly unbound".
    auto unbound = uimodel::KeymapModel{uimodel::KeymapBindings{{"applicator.unbind", {}}}};
    applyKeymapAccelerators(application, unbound);

    CHECK(application.get_accels_for_action("win.applicator.unbind").empty());
  }
} // namespace ao::gtk::test
