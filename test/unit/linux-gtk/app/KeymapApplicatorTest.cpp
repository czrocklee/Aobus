// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/KeymapApplicator.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::gtk::test
{
  namespace
  {
    uimodel::input::KeyChord chord(std::string const& text)
    {
      auto const optChord = uimodel::input::KeyChord::parse(text);
      REQUIRE(optChord.has_value());
      return *optChord;
    }
  }

  TEST_CASE("applyKeymapAccelerators installs accelerators for bound actions", "[gtk][unit][app][accel]")
  {
    auto const appPtr = ensureGtkApplication();

    auto model = uimodel::input::KeymapModel{uimodel::input::KeymapBindings{{"applicator.install", {chord("Ctrl+P")}}}};
    applyKeymapAccelerators(*appPtr, model);

    CHECK_FALSE(appPtr->get_accels_for_action("win.applicator.install").empty());
  }

  TEST_CASE("applyKeymapAccelerators clears accelerators dropped from the keymap", "[gtk][unit][app][accel]")
  {
    auto const appPtr = ensureGtkApplication();

    // Bind an action whose id is absent from the default keymap, then re-apply a keymap that no
    // longer mentions it (mirrors resetToDefault on a binding with no shipped default, which erases
    // the entry). The stale accelerator must not survive the re-apply.
    auto bound = uimodel::input::KeymapModel{uimodel::input::KeymapBindings{{"applicator.drop", {chord("Ctrl+J")}}}};
    applyKeymapAccelerators(*appPtr, bound);
    REQUIRE_FALSE(appPtr->get_accels_for_action("win.applicator.drop").empty());

    auto dropped = uimodel::input::KeymapModel{uimodel::input::KeymapBindings{}};
    applyKeymapAccelerators(*appPtr, dropped);

    CHECK(appPtr->get_accels_for_action("win.applicator.drop").empty());
  }

  TEST_CASE("applyKeymapAccelerators clears an explicitly unbound action", "[gtk][unit][app][accel]")
  {
    auto const appPtr = ensureGtkApplication();

    auto bound = uimodel::input::KeymapModel{uimodel::input::KeymapBindings{{"applicator.unbind", {chord("Ctrl+K")}}}};
    applyKeymapAccelerators(*appPtr, bound);
    REQUIRE_FALSE(appPtr->get_accels_for_action("win.applicator.unbind").empty());

    // An empty chord list (still present in the keymap) means "explicitly unbound".
    auto unbound = uimodel::input::KeymapModel{uimodel::input::KeymapBindings{{"applicator.unbind", {}}}};
    applyKeymapAccelerators(*appPtr, unbound);

    CHECK(appPtr->get_accels_for_action("win.applicator.unbind").empty());
  }
} // namespace ao::gtk::test
