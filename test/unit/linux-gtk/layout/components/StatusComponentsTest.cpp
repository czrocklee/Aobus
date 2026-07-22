// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/status/StatusRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>

namespace ao::gtk::layout::test
{
  TEST_CASE("StatusComponents - status bar components register status descriptors", "[gtk][unit][layout][status]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.status_test");

    auto registry = ComponentRegistry{};
    registerStatusComponents(registry);

    auto const optDesc = registry.descriptor("status.activityStatus");
    REQUIRE(optDesc);
    CHECK(optDesc->displayName == "Activity Status");

    auto const optSelectionDesc = registry.descriptor("status.selectionInfo");
    REQUIRE(optSelectionDesc);
    CHECK(optSelectionDesc->displayName == "Selection Info");

    CHECK_FALSE(registry.descriptor("status.statusSlot").has_value());
    CHECK_FALSE(registry.descriptor("status.notificationCenter").has_value());
  }
} // namespace ao::gtk::layout::test
